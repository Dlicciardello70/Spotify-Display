# ESP32-S3 Spotify Now Playing Display

A self-contained Spotify now-playing display built for an ESP32-S3 2.8" TFT touchscreen development board. The device connects directly to Wi-Fi, talks to Spotify's Web API, downloads album art, and renders a polished portrait UI with track metadata, playback state, volume, and progress.

This project was built as an embedded systems portfolio piece: it combines API integration, constrained-device graphics, token persistence, image decoding, display driver configuration, and practical reliability fixes for real hardware.

## Highlights

- Runs directly on the ESP32-S3 with no always-on PC server.
- Shows current playlist/context, album art, song title, artist, volume, play/pause state, elapsed time, total duration, and progress.
- Downloads and decodes Spotify album art on-device with `JPEGDEC`.
- Uses PSRAM for larger album-art downloads.
- Smoothly updates progress locally between Spotify API polls.
- Scrolls long song and artist names without shifting the UI.
- Reduces flicker by only redrawing UI regions that changed.
- Turns the TFT backlight off after 30 minutes paused or 30 minutes with nothing playing.
- Stores rotated Spotify refresh tokens in ESP32 preferences/NVS.
- Includes a PC preview tool for testing the Spotify API flow before flashing hardware.

## Tech Stack

- **Hardware:** ESP32-S3, 240x320 SPI TFT LCD, optional capacitive touch board
- **Firmware:** Arduino/C++
- **Graphics:** `Arduino_GFX_Library`
- **Image Decoding:** `JPEGDEC`
- **JSON Parsing:** `ArduinoJson`
- **Networking:** ESP32 Wi-Fi, HTTPS requests to Spotify
- **Auth:** Spotify Authorization Code with PKCE, refresh-token based access
- **Preview Tool:** Python local preview app

## Target Hardware

The main sketch is configured for the LCDWiki/GoodDisplay ES3C28P style board:

- ESP32-S3
- 2.8" 240x320 TFT display
- ILI9341 display driver
- 16 MB flash
- 8 MB PSRAM

Display pins used by the sketch:

```cpp
#define TFT_MOSI 11
#define TFT_MISO 13
#define TFT_SCLK 12
#define TFT_CS 10
#define TFT_DC 46
#define TFT_RST GFX_NOT_DEFINED
#define TFT_BL 45
```

## Project Structure

```text
spotify_tft_esp32_s3/
  SpotifyNowPlaying/
    SpotifyNowPlaying.ino      Main ESP32 firmware
    secrets.h                  Local Wi-Fi/Spotify secrets, not for publishing
  pc_simulator/
    spotify_pc_preview.py      Browser-based Spotify UI preview
    config.json                Local simulator config, not for publishing
  tools/
    get_spotify_refresh_token.py
  README.md
```

## What The Firmware Does

1. Connects to Wi-Fi.
2. Refreshes the Spotify access token.
3. Polls Spotify's player endpoint every few seconds.
4. Downloads the largest useful album image.
5. Decodes JPEG pixels into the album-art region.
6. Draws a stable portrait UI:
   - playlist/context header
   - rounded album art
   - song title
   - artist name
   - volume and play/pause state
   - progress bar and timestamps
7. Updates progress locally for smooth movement.
8. Sleeps the backlight when paused or idle for 30 minutes.

## Arduino IDE Setup

Install these libraries from Arduino IDE Library Manager:

- `ArduinoJson`
- `Arduino_GFX_Library`
- `JPEGDEC`

Use these board settings:

- **Board:** `ESP32S3 Dev Module`
- **USB CDC On Boot:** `Enabled`
- **PSRAM:** `OPI PSRAM` or `Enabled`
- **Flash Size:** `16MB`
- **Partition Scheme:** `Huge APP` or the largest app option
- **CPU Frequency:** `240MHz`
- **Flash Frequency:** `80MHz`
- **Flash Mode:** `QIO`, or `DIO` if boot/upload is unreliable
- **Upload Speed:** `921600`, or lower if upload fails

PSRAM is important. If Serial Monitor shows `total psram=0`, large album art may fail to download or decode.

## Spotify App Setup

1. Create an app in the Spotify Developer Dashboard.
2. Add this redirect URI:

```text
http://127.0.0.1:8888/callback
```

3. Copy the Spotify Client ID.
4. Run the token helper:

```powershell
python .\tools\get_spotify_refresh_token.py
```

5. Log in through the browser window.
6. Copy the refresh token into `SpotifyNowPlaying/secrets.h`.

The sketch does not use a Spotify client secret. It uses the PKCE flow, which is better suited for a device/client-side project.

## Local Secrets

Create `SpotifyNowPlaying/secrets.h` locally:

```cpp
#pragma once

#define WIFI_SSID "your_wifi_name"
#define WIFI_PASSWORD "your_wifi_password"
#define SPOTIFY_CLIENT_ID "your_spotify_client_id"
#define SPOTIFY_REFRESH_TOKEN "your_refresh_token"
```

Do not publish `secrets.h`, `pc_simulator/config.json`, or any refresh tokens.

## PC Preview

The PC simulator is useful for testing Spotify credentials and UI layout before flashing the ESP32.

```powershell
cd C:\Users\flame\Downloads\spotify_tft_esp32_s3
python .\pc_simulator\spotify_pc_preview.py
```

Then open:

```text
http://127.0.0.1:8899
```

The final firmware does not require this preview server. It is only a development tool.

## Reliability Details

This project includes several hardware-focused fixes that came from testing on the actual board:

- Correct ILI9341 SPI pin mapping for the display.
- Backlight control through the board's TFT backlight pin.
- PSRAM diagnostics printed at boot.
- Retry logic for album-art downloads.
- Support for image responses with missing `Content-Length`.
- Guard against tiny Spotify thumbnails when better art is available.
- Detection for JPEGs that decode as solid green on the ESP32.
- Cached idle screens to avoid flicker while Spotify reports nothing playing.
- Reduced redraw regions for smoother progress updates.

## Portfolio Talking Points

This project is useful to discuss in interviews because it shows:

- Building a complete embedded product loop from API to display.
- Debugging real hardware issues with serial logging and incremental tests.
- Managing limited memory with PSRAM and bounded downloads.
- Designing a UI for a tiny screen where redraw cost matters.
- Handling OAuth refresh-token rotation safely on-device.
- Making pragmatic tradeoffs when third-party JPEGs decode inconsistently on microcontroller hardware.

## Security Notes

This is a personal display project. For production use:

- Do not commit Wi-Fi credentials or Spotify refresh tokens.
- Replace `client.setInsecure()` with proper TLS certificate validation.
- Consider a safer token provisioning flow for deployed devices.

## Status

Working on the target ESP32-S3 TFT board with Spotify now-playing data, album art, playlist/context header, progress, volume, play/pause state, flicker reduction, and idle backlight sleep.
