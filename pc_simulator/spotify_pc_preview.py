#!/usr/bin/env python3
import json
import mimetypes
import pathlib
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    from PIL import Image
except ImportError:
    Image = None

# ---------------- Local preview configuration ----------------
# The preview reads Spotify credentials from config.json and serves a browser UI on PORT.
ROOT = pathlib.Path(__file__).resolve().parent
CONFIG_PATH = ROOT / "config.json"
HOST = "0.0.0.0"
PORT = 8899
ART_SIZE = 208

# ---------------- Spotify/cache state ----------------
# These globals mirror the ESP32 sketch state enough to preview the UI on a PC.
access_token = ""
access_token_expires_at = 0.0
config_mtime = 0.0
last_album_art = b""
last_album_art_type = "image/jpeg"
last_album_art_url = ""


# Load and validate the local Spotify preview config.
def load_config():
    global config_mtime
    if not CONFIG_PATH.exists():
        raise SystemExit(
            f"Missing {CONFIG_PATH}\n"
            f"Copy config.json.example to config.json and fill in your Spotify values."
        )
    with CONFIG_PATH.open("r", encoding="utf-8") as f:
        config = json.load(f)
    for key in ("client_id", "refresh_token"):
        if not config.get(key) or config[key].startswith("your-"):
            raise SystemExit(f"Set {key!r} in {CONFIG_PATH}")
    config_mtime = CONFIG_PATH.stat().st_mtime
    return config


CONFIG = load_config()


# Save config.json after Spotify rotates the refresh token.
def save_config():
    global config_mtime
    with CONFIG_PATH.open("w", encoding="utf-8") as f:
        json.dump(CONFIG, f, indent=2)
        f.write("\n")
    config_mtime = CONFIG_PATH.stat().st_mtime


# Reload config.json while the preview is running if the user edits it.
def reload_config_if_changed():
    global CONFIG, access_token, access_token_expires_at, config_mtime
    latest_mtime = CONFIG_PATH.stat().st_mtime
    if latest_mtime == config_mtime:
        return
    CONFIG = load_config()
    access_token = ""
    access_token_expires_at = 0.0


# POST x-www-form-urlencoded data and parse a Spotify JSON response.
def post_form(url, fields):
    body = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as res:
            return json.loads(res.read().decode("utf-8"))
    except urllib.error.HTTPError as err:
        body = err.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Spotify token request failed: HTTP {err.code}: {body}") from err


# GET a JSON endpoint while preserving Spotify's 204 "nothing playing" response.
def get_json(url, headers=None):
    req = urllib.request.Request(url, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=15) as res:
            if res.status == 204:
                return 204, None
            return res.status, json.loads(res.read().decode("utf-8"))
    except urllib.error.HTTPError as err:
        body = err.read().decode("utf-8", errors="replace")
        return err.code, {"error": body}


# Refresh the Spotify access token and persist any rotated refresh token.
def refresh_access_token():
    global access_token, access_token_expires_at
    reload_config_if_changed()
    if access_token and time.time() < access_token_expires_at - 60:
        return access_token
    token = post_form(
        "https://accounts.spotify.com/api/token",
        {
            "grant_type": "refresh_token",
            "refresh_token": CONFIG["refresh_token"],
            "client_id": CONFIG["client_id"],
        },
    )
    access_token = token["access_token"]
    access_token_expires_at = time.time() + int(token.get("expires_in", 3600))
    if token.get("refresh_token") and token["refresh_token"] != CONFIG["refresh_token"]:
        CONFIG["refresh_token"] = token["refresh_token"]
        save_config()
        print("Spotify rotated the refresh token; saved the new one to pc_simulator/config.json")
    return access_token


# Download the current album image once and cache it for the browser preview.
def cache_album_art(url):
    global last_album_art, last_album_art_type, last_album_art_url
    if not url or url == last_album_art_url:
        return
    req = urllib.request.Request(url, headers={"User-Agent": "spotify-pc-preview"})
    with urllib.request.urlopen(req, timeout=20) as res:
        last_album_art = res.read()
        last_album_art_type = res.headers.get_content_type() or mimetypes.guess_type(url)[0] or "image/jpeg"
        last_album_art_url = url


# Optional dev helper: convert an image URL to raw RGB565 bytes for ESP32 experiments.
def rgb565_bytes_from_url(url):
    if Image is None:
        raise RuntimeError("Pillow is not available. Run with the bundled Codex Python or install Pillow.")

    req = urllib.request.Request(url, headers={"User-Agent": "spotify-pc-preview"})
    with urllib.request.urlopen(req, timeout=20) as res:
        with Image.open(res) as img:
            img = img.convert("RGB")
            canvas = Image.new("RGB", (max(img.size), max(img.size)), (0, 0, 0))
            canvas.paste(img, ((canvas.width - img.width) // 2, (canvas.height - img.height) // 2))
            img = canvas.resize((ART_SIZE, ART_SIZE), Image.Resampling.LANCZOS)
            out = bytearray(ART_SIZE * ART_SIZE * 2)
            i = 0
            for r, g, b in img.getdata():
                rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                out[i] = (rgb565 >> 8) & 0xFF
                out[i + 1] = rgb565 & 0xFF
                i += 2
            return bytes(out)


# Fetch Spotify's current player state and convert it into the UI JSON shape.
def currently_playing():
    token = refresh_access_token()
    auth_headers = {"Authorization": f"Bearer {token}"}
    status, payload = get_json(
        "https://api.spotify.com/v1/me/player?additional_types=track",
        auth_headers,
    )
    if status == 204:
        return {"state": "idle", "message": "Nothing is currently playing."}
    if status != 200:
        return {"state": "error", "status": status, "payload": payload}

    item = payload.get("item") or {}
    if not item:
        return {"state": "idle", "message": "Spotify returned no track item."}

    images = (((item.get("album") or {}).get("images")) or [])
    image_url = images[1]["url"] if len(images) > 1 else (images[0]["url"] if images else "")
    if image_url:
        cache_album_art(image_url)

    context = payload.get("context") or {}
    context_name = "Spotify"
    if context.get("href"):
        context_status, context_payload = get_json(context["href"], auth_headers)
        if context_status == 200 and context_payload:
            context_name = context_payload.get("name") or context_payload.get("title") or context.get("type", "Spotify").title()
        elif context.get("type"):
            context_name = context["type"].title()
    elif context.get("type"):
        context_name = context["type"].title()

    artists = ", ".join(a.get("name", "") for a in item.get("artists", []) if a.get("name"))
    return {
        "state": "playing" if payload.get("is_playing") else "paused",
        "context_name": context_name,
        "track_id": item.get("id", ""),
        "title": item.get("name", "Unknown title"),
        "artist": artists or "Unknown artist",
        "duration_ms": item.get("duration_ms") or 0,
        "progress_ms": payload.get("progress_ms") or 0,
        "volume_percent": ((payload.get("device") or {}).get("volume_percent")),
        "synced_at_ms": int(time.time() * 1000),
        "album_art": "/album-art",
        "poll_seconds": CONFIG.get("poll_seconds", 5),
    }


# ---------------- Browser preview UI ----------------
# The HTML/CSS/JS below mirrors the 240x320 TFT layout in a resizable browser frame.
INDEX_HTML = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>ESP32 Spotify Preview</title>
  <style>
    :root { color-scheme: dark; font-family: "CircularSp", "Spotify Circular", "Circular Std", "Avenir Next", Montserrat, "Segoe UI", Arial, sans-serif; background: #111; color: #f5f5f5; }
    body { margin: 0; min-height: 100vh; display: grid; place-items: center; background: #141414; }
    .screen { width: 410px; max-width: calc(100vw - 32px); aspect-ratio: 3 / 4; background: #080f0d; border: 1px solid #2b332f; box-shadow: 0 24px 70px #0009; overflow: hidden; }
    .header { height: 48px; display: flex; align-items: center; justify-content: center; padding: 0 12px; background: #080f0d; color: #f5f5f5; font-size: 16px; font-weight: 800; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; text-align: center; box-sizing: border-box; }
    .main { height: calc(100% - 112px); display: flex; flex-direction: column; padding: 0 25px; box-sizing: border-box; }
    .art { width: 100%; aspect-ratio: 1 / 1; object-fit: cover; background: #222724; display: block; border-radius: 8px; }
    .copy { min-width: 0; flex: 1; display: flex; flex-direction: column; justify-content: center; padding: 8px 0 10px; box-sizing: border-box; transform: translateY(8px); }
    .title, .artist { overflow: hidden; white-space: nowrap; }
    .title { height: 32px; font-size: 25px; line-height: 32px; font-weight: 800; }
    .meta-row { display: flex; align-items: center; gap: 10px; height: 22px; margin-top: 2px; }
    .artist { flex: 1 1 auto; min-width: 0; height: 22px; color: #c8d2cc; font-size: 16px; line-height: 22px; }
    .marquee-inner { display: inline-block; min-width: max-content; will-change: transform; transform: translateX(0); }
    .progress-wrap { height: 64px; padding: 12px 25px 0; box-sizing: border-box; position: relative; }
    .state-icon { width: 22px; height: 22px; color: #f5f5f5; display: grid; place-items: center; flex: 0 0 auto; }
    .state-icon svg { width: 22px; height: 22px; display: block; fill: currentColor; }
    .volume { width: 72px; height: 22px; display: flex; align-items: center; color: #c8d2cc; flex: 0 0 auto; transform: translateY(3px); }
    .state-icon { transform: translateY(2px); }
    .volume-bar { width: 72px; height: 5px; border-radius: 999px; background: #222724; overflow: hidden; }
    .volume-fill { width: 0%; height: 100%; border-radius: inherit; background: #c8d2cc; }
    .bar { height: 8px; background: #222724; border-radius: 999px; overflow: hidden; }
    .fill { width: 0%; height: 100%; background: #1ed760; }
    .times { display: flex; justify-content: space-between; color: #c8d2cc; font-size: 12px; margin-top: 6px; }
    .empty { grid-column: 1 / -1; display: grid; place-items: center; font-size: 26px; color: #f5f5f5; }
  </style>
</head>
<body>
  <section class="screen">
    <div class="header" id="header">Spotify</div>
    <main class="main" id="main"><div class="empty">Loading...</div></main>
    <div class="progress-wrap">
      <div class="bar"><div class="fill" id="fill"></div></div>
      <div class="times"><span id="elapsed">0:00</span><span id="duration">0:00</span></div>
    </div>
  </section>
  <script>
    let current = null;
    let pollMs = 5000;
    let renderedTrackKey = "";

    const fmt = ms => {
      const s = Math.max(0, Math.floor(ms / 1000));
      return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
    };

    // Estimate progress locally between Spotify polls so the bar moves smoothly.
    function progressMs() {
      if (!current) return 0;
      const base = current.progress_ms || 0;
      if (current.state !== "playing") return base;
      return Math.min(current.duration_ms || 0, base + (Date.now() - current.synced_at_ms));
    }

    // Scroll long song/artist text like the ESP32 marquee behavior.
    function setMarquee(el, text, pixelsPerSecond) {
      if (el.dataset.text === text) return;
      el.dataset.text = text;
      el.replaceChildren();
      const inner = document.createElement("span");
      inner.className = "marquee-inner";
      inner.textContent = text;
      el.appendChild(inner);

      requestAnimationFrame(() => {
        const overflow = inner.scrollWidth - el.clientWidth;
        if (overflow <= 0) return;

        const travelMs = Math.max(2800, Math.min(12000, (overflow / pixelsPerSecond) * 1000));
        const holdMs = 1000;
        const totalMs = travelMs + holdMs * 2;
        inner.animate(
          [
            { transform: "translateX(0)", offset: 0 },
            { transform: "translateX(0)", offset: holdMs / totalMs },
            { transform: `translateX(-${overflow}px)`, offset: (holdMs + travelMs) / totalMs },
            { transform: `translateX(-${overflow}px)`, offset: 0.999 },
            { transform: "translateX(0)", offset: 1 }
          ],
          {
            duration: totalMs,
            easing: "linear",
            iterations: Infinity
          }
        );
      });
    }

    // Update progress, timestamps, volume, and play/pause icon without rebuilding the whole screen.
    function drawProgress() {
      const ms = progressMs();
      const duration = current?.duration_ms || 0;
      document.getElementById("fill").style.width = duration ? `${(ms / duration) * 100}%` : "0%";
      document.getElementById("elapsed").textContent = fmt(ms);
      document.getElementById("duration").textContent = fmt(duration);
      const icon = document.getElementById("stateIcon");
      if (icon) {
        icon.innerHTML = current?.state === "playing"
          ? '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="6" y="5" width="4" height="14" rx="1"></rect><rect x="14" y="5" width="4" height="14" rx="1"></rect></svg>'
          : '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M8 5v14l11-7z"></path></svg>';
      }
      const volumeFill = document.getElementById("volumeFill");
      if (volumeFill) {
        const volume = Number.isFinite(current?.volume_percent) ? current.volume_percent : 0;
        volumeFill.style.width = `${Math.max(0, Math.min(100, volume))}%`;
      }
    }

    // Poll the local Python API and redraw the preview only when the track identity changes.
    async function fetchTrack() {
      const res = await fetch("/api/currently-playing");
      current = await res.json();
      pollMs = (current.poll_seconds || 5) * 1000;
      const header = document.getElementById("header");
      header.textContent = current.context_name || "Spotify";
      const main = document.getElementById("main");
      if (current.state === "error") {
        header.textContent = "Spotify error";
        main.innerHTML = `<div class="empty">Check the terminal for details.</div>`;
        return;
      }
      if (current.state === "idle") {
        renderedTrackKey = "";
        header.textContent = "Spotify";
        main.innerHTML = `<div class="empty">${current.message}</div>`;
        return;
      }
      const trackKey = `${current.track_id}|${current.title}|${current.artist}|${current.context_name}`;
      if (trackKey !== renderedTrackKey) {
        renderedTrackKey = trackKey;
        main.innerHTML = `
          <img class="art" src="${current.album_art}?t=${encodeURIComponent(current.track_id)}" alt="">
          <div class="copy">
            <div class="title"></div>
            <div class="meta-row">
              <div class="artist"></div>
              <div class="volume" id="volume"><div class="volume-bar"><div class="volume-fill" id="volumeFill"></div></div></div>
              <div class="state-icon" id="stateIcon"></div>
            </div>
          </div>`;
        setMarquee(main.querySelector(".title"), current.title, 42);
        setMarquee(main.querySelector(".artist"), current.artist, 27);
      }
      drawProgress();
    }

    fetchTrack();
    setInterval(() => fetchTrack().catch(console.error), pollMs);
    function animateProgress() {
      drawProgress();
      requestAnimationFrame(animateProgress);
    }
    requestAnimationFrame(animateProgress);
  </script>
</body>
</html>
"""


# ---------------- HTTP server ----------------
# Serves the preview page, current Spotify JSON, cached album art, and optional RGB565 helper output.
class Handler(BaseHTTPRequestHandler):
    # Keep request logs visible in the terminal for quick debugging.
    def log_message(self, fmt, *args):
        print(fmt % args)

    # Send a small response body with a fixed content type.
    def send_bytes(self, status, body, content_type):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # Route browser/API requests for the preview app.
    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index.html"):
            self.send_bytes(200, INDEX_HTML.encode("utf-8"), "text/html; charset=utf-8")
            return
        if self.path.startswith("/api/currently-playing"):
            try:
                body = json.dumps(currently_playing()).encode("utf-8")
            except Exception as exc:
                print(f"Spotify request failed: {exc}")
                body = json.dumps({"state": "error", "status": "local", "payload": str(exc)}).encode("utf-8")
            self.send_bytes(200, body, "application/json")
            return
        if self.path.startswith("/album-art-rgb565"):
            parsed = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(parsed.query)
            url = qs.get("url", [""])[0]
            if not url:
                self.send_bytes(400, b"Missing url", "text/plain")
                return
            try:
                self.send_bytes(200, rgb565_bytes_from_url(url), "application/octet-stream")
            except Exception as exc:
                print(f"Album conversion failed: {exc}")
                self.send_bytes(500, str(exc).encode("utf-8"), "text/plain")
            return
        if self.path.startswith("/album-art"):
            if not last_album_art:
                self.send_bytes(404, b"", "text/plain")
                return
            self.send_bytes(200, last_album_art, last_album_art_type)
            return
        self.send_bytes(404, b"Not found", "text/plain")


# Start the local preview server.
def main():
    print(f"Open http://{HOST}:{PORT}")
    print("Press Ctrl+C to stop.")
    ThreadingHTTPServer((HOST, PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
