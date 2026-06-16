#!/usr/bin/env python3
import base64
import hashlib
import http.server
import json
import secrets
import socketserver
import urllib.parse
import urllib.request
import webbrowser

CLIENT_ID = "398315af4a574d9795114827a59f3ee4"
REDIRECT_URI = "http://127.0.0.1:8888/callback"
SCOPES = "user-read-currently-playing user-read-playback-state playlist-read-private"


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode().rstrip("=")


def post_form(url: str, fields: dict) -> dict:
    body = urllib.parse.urlencode(fields).encode()
    req = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        method="POST",
    )
    with urllib.request.urlopen(req) as res:
        return json.loads(res.read().decode())


def main():
    if CLIENT_ID.startswith("PASTE_"):
        raise SystemExit("Edit CLIENT_ID at the top of this file first.")

    verifier = b64url(secrets.token_bytes(64))
    challenge = b64url(hashlib.sha256(verifier.encode()).digest())
    state = secrets.token_urlsafe(24)
    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            parsed = urllib.parse.urlparse(self.path)
            qs = urllib.parse.parse_qs(parsed.query)
            if parsed.path != "/callback":
                self.send_response(404)
                self.end_headers()
                return
            if qs.get("state", [""])[0] != state:
                result["error"] = "State did not match."
            elif "error" in qs:
                result["error"] = qs["error"][0]
            else:
                result["code"] = qs.get("code", [""])[0]

            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(b"<h1>Spotify token captured.</h1><p>You can close this tab.</p>")

    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(
        {
            "response_type": "code",
            "client_id": CLIENT_ID,
            "scope": SCOPES,
            "redirect_uri": REDIRECT_URI,
            "state": state,
            "code_challenge_method": "S256",
            "code_challenge": challenge,
        }
    )

    print("Opening Spotify login...")
    webbrowser.open(auth_url)

    with socketserver.TCPServer(("127.0.0.1", 8888), Handler) as server:
        server.handle_request()

    if result.get("error"):
        raise SystemExit(result["error"])
    if not result.get("code"):
        raise SystemExit("No authorization code received.")

    token = post_form(
        "https://accounts.spotify.com/api/token",
        {
            "grant_type": "authorization_code",
            "code": result["code"],
            "redirect_uri": REDIRECT_URI,
            "client_id": CLIENT_ID,
            "code_verifier": verifier,
        },
    )

    print("\nRefresh token:\n")
    print(token["refresh_token"])
    print("\nPaste it into SpotifyNowPlaying/secrets.h as SPOTIFY_REFRESH_TOKEN.")


if __name__ == "__main__":
    main()
