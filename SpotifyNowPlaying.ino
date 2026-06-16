#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>

#include "secrets.h"

// ---------------- Hardware display wiring ----------------
// LCDWiki/GoodDisplay ES3C28P: ESP32-S3 2.8" 240x320 capacitive touch board.
// https://www.lcdwiki.com/2.8inch_ESP32-S3_Display
#define TFT_MOSI 11
#define TFT_MISO 13
#define TFT_SCLK 12
#define TFT_CS 10
#define TFT_DC 46
#define TFT_RST GFX_NOT_DEFINED
#define TFT_BL 45
#define TFT_BACKLIGHT_ON HIGH

static Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);
static Arduino_GFX *gfx = new Arduino_ILI9341(bus, TFT_RST, 0);

// If colors/rotation ever look wrong, the final constructor argument is the ILI9341 rotation value.

// ---------------- Layout ----------------
// Fixed portrait layout for the 240x320 display.
static constexpr int16_t SCREEN_W = 240;
static constexpr int16_t SCREEN_H = 320;
static constexpr int16_t ART_X = 16;
static constexpr int16_t ART_Y = 32;
static constexpr int16_t ART_SIZE = 208;
static constexpr int16_t TEXT_X = 16;
static constexpr int16_t TEXT_W = 208;
static constexpr uint16_t BG = 0x0000;       // black
static constexpr uint16_t PANEL = 0x1082;    // dark gray
static constexpr uint16_t FG = 0xFFFF;       // white
static constexpr uint16_t MUTED = 0xBDF7;    // light gray
static constexpr uint16_t ACCENT = 0x07E0;   // green

// ---------------- Spotify/player state ----------------
// These values are updated by Spotify polling and reused by the draw functions.
static String accessToken;
static String refreshToken;
static uint32_t tokenExpiresAtMs = 0;
static String lastTrackId;
static String currentTitle;
static String currentArtist;
static String currentContextName = "Spotify";
static String cachedContextUri;
static String cachedContextName = "Spotify";
static uint32_t contextRetryAfterMs = 0;
static int currentDurationMs = 0;
static int currentVolumePercent = 0;
static int lastProgressMs = 0;
static uint32_t lastProgressSyncMs = 0;
static bool isPlaying = false;
static uint32_t pausedSinceMs = 0;
static uint32_t idleSinceMs = 0;
static bool screenSleeping = false;
static String lastIdleMessage;
static int lastDrawnProgressFill = -1;
static int lastDrawnVolumePercent = -1;
static bool lastDrawnPlaying = false;
static bool controlsDrawn = false;
static uint16_t titleScroll = 0;
static uint16_t artistScroll = 0;
static uint8_t titlePauseTicks = 3;
static uint8_t artistPauseTicks = 3;
static String lastVisibleTitle;
static String lastVisibleArtist;

// ---------------- Album-art decoding state ----------------
// JPEGDEC calls back into the sketch line by line while decoding.
static JPEGDEC jpeg;
static Preferences prefs;
static uint8_t *imageBuffer = nullptr;
static size_t imageBufferLen = 0;
static int jpegSourceW = 0;
static int jpegSourceH = 0;
static uint8_t jpegScaleDiv = 1;
static bool albumEnhanceColors = true;
static bool albumScanOnly = false;
static uint32_t albumSampleCount = 0;
static uint32_t albumGreenDominantCount = 0;

// ---------------- Timing and memory limits ----------------
// Sleep timers protect the TFT from staying lit forever, and byte limits keep album downloads bounded.
static constexpr uint32_t PAUSE_SLEEP_AFTER_MS = 30UL * 60UL * 1000UL;
static constexpr uint32_t IDLE_SLEEP_AFTER_MS = 30UL * 60UL * 1000UL;
static constexpr size_t MAX_ALBUM_ART_BYTES = 500000;
static constexpr size_t UNKNOWN_ALBUM_ART_BYTES = 300000;

// Print heap and PSRAM details so display/art failures can be diagnosed from Serial Monitor.
void printMemoryStatus(const char *label) {
  Serial.printf("%s memory: heap free=%u largest heap block=%u psram free=%u total psram=%u\n",
                label,
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap(),
                (unsigned)ESP.getFreePsram(),
                (unsigned)ESP.getPsramSize());
}

// Turn only the TFT backlight on/off while keeping the ESP32 running.
void setScreenSleeping(bool sleeping) {
  if (screenSleeping == sleeping) return;
  screenSleeping = sleeping;
  digitalWrite(TFT_BL, sleeping ? !TFT_BACKLIGHT_ON : TFT_BACKLIGHT_ON);
  Serial.println(sleeping ? "Screen backlight off after 30 minutes inactive." : "Screen backlight on.");
}

// Track paused playback time and sleep the screen after the pause timeout.
bool updatePauseSleepTimer() {
  if (isPlaying) {
    bool wasSleeping = screenSleeping;
    pausedSinceMs = 0;
    idleSinceMs = 0;
    setScreenSleeping(false);
    return wasSleeping;
  }

  if (pausedSinceMs == 0) pausedSinceMs = millis();
  if (millis() - pausedSinceMs >= PAUSE_SLEEP_AFTER_MS) {
    setScreenSleeping(true);
  }
  return false;
}

// Track the "nothing playing" state and sleep the screen after the idle timeout.
bool updateIdleSleepTimer() {
  bool wasSleeping = screenSleeping;
  if (idleSinceMs == 0) idleSinceMs = millis();
  pausedSinceMs = 0;
  isPlaying = false;

  if (millis() - idleSinceMs >= IDLE_SLEEP_AFTER_MS) {
    setScreenSleeping(true);
  }
  return wasSleeping && !screenSleeping;
}

// Return true when a pixel is inside the rounded album-art mask.
bool insideRoundedArt(int16_t x, int16_t y) {
  static constexpr int16_t r = 8;
  int16_t localX = x - ART_X;
  int16_t localY = y - ART_Y;
  if (localX < 0 || localY < 0 || localX >= ART_SIZE || localY >= ART_SIZE) return false;

  if (localX >= r && localX < ART_SIZE - r) return true;
  if (localY >= r && localY < ART_SIZE - r) return true;

  int16_t cx = localX < r ? r : ART_SIZE - r - 1;
  int16_t cy = localY < r ? r : ART_SIZE - r - 1;
  int16_t dx = localX - cx;
  int16_t dy = localY - cy;
  return dx * dx + dy * dy <= r * r;
}

// Clamp a color channel to the 0-255 byte range.
uint8_t clamp8(int value) {
  if (value < 0) return 0;
  if (value > 255) return 255;
  return value;
}

// Darken, contrast-boost, and saturate RGB565 album pixels for this bright TFT.
uint16_t enhanceAlbumColor(uint16_t color) {
  int r = ((color >> 11) & 0x1F) * 255 / 31;
  int g = ((color >> 5) & 0x3F) * 255 / 63;
  int b = (color & 0x1F) * 255 / 31;

  // Slightly darken and deepen colors to compensate for the bright TFT/JPEG path.
  r = (r * 88) / 100;
  g = (g * 88) / 100;
  b = (b * 88) / 100;

  r = ((r - 110) * 125) / 100 + 110;
  g = ((g - 110) * 125) / 100 + 110;
  b = ((b - 110) * 125) / 100 + 110;

  int luma = (r * 77 + g * 150 + b * 29) >> 8;
  r = luma + ((r - luma) * 135) / 100;
  g = luma + ((g - luma) * 135) / 100;
  b = luma + ((b - luma) * 135) / 100;

  r = clamp8(r);
  g = clamp8(g);
  b = clamp8(b);

  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Detect the all-green decoder failure that some Spotify JPEGs trigger.
bool isGreenDominant565(uint16_t color) {
  int r8 = ((color >> 11) & 0x1F) * 255 / 31;
  int g8 = ((color >> 5) & 0x3F) * 255 / 63;
  int b8 = (color & 0x1F) * 255 / 31;
  return g8 > 90 && g8 > r8 * 2 && g8 > b8 * 2;
}

// Percent-encode strings for Spotify form bodies and query-safe values.
String urlEncode(const String &s) {
  String out;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Connect to Wi-Fi and draw a simple connection status on the TFT.
void connectWiFi() {
  Serial.printf("Connecting to Wi-Fi SSID: %s\n", WIFI_SSID);
  WiFi.disconnect(true, true);
  delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  gfx->fillScreen(BG);
  gfx->setTextColor(FG, BG);
  gfx->setTextSize(2);
  gfx->setCursor(16, 90);
  gfx->print("Connecting Wi-Fi");

  uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 30000) {
    delay(350);
    Serial.print(".");
    gfx->print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.printf("Wi-Fi failed. Status code: %d\n", WiFi.status());
    drawIdle("Wi-Fi failed");
  }
}

// Refresh the Spotify access token, saving any rotated refresh token in ESP32 preferences.
bool refreshAccessToken() {
  if (accessToken.length() && millis() < tokenExpiresAtMs - 60000) return true;
  if (!refreshToken.length()) refreshToken = SPOTIFY_REFRESH_TOKEN;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=refresh_token";
  body += "&refresh_token=" + urlEncode(refreshToken);
  body += "&client_id=" + urlEncode(SPOTIFY_CLIENT_ID);

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("Token refresh failed: HTTP %d\n%s\n", code, http.getString().c_str());
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return false;

  accessToken = doc["access_token"].as<String>();
  int expiresIn = doc["expires_in"] | 3600;
  tokenExpiresAtMs = millis() + (expiresIn * 1000UL);

  String rotatedRefreshToken = doc["refresh_token"].as<String>();
  if (rotatedRefreshToken.length() && rotatedRefreshToken != refreshToken) {
    refreshToken = rotatedRefreshToken;
    prefs.putString("refresh", refreshToken);
    Serial.println("Spotify rotated refresh token; saved it to ESP32 preferences.");
  }

  return accessToken.length() > 0;
}

// Draw the top header, usually the current playlist/context name.
void drawHeader(const char *text) {
  gfx->fillRect(0, 0, SCREEN_W, ART_Y, BG);
  gfx->setTextSize(1);
  gfx->setTextColor(FG, BG);
  String visible = text;
  if (visible.length() > 30) visible = visible.substring(0, 27) + "...";
  int16_t textW = visible.length() * 6;
  int16_t x = max(0, (SCREEN_W - textW) / 2);
  gfx->setCursor(x, 9);
  gfx->print(visible);
  gfx->setCursor(x + 1, 9);
  gfx->print(visible);
}

// Draw wrapped text within a fixed-width area; kept for simple multi-line messages.
void drawWrappedText(const String &text, int16_t x, int16_t y, int16_t maxW, uint16_t color, uint8_t size, uint8_t lines) {
  gfx->setTextSize(size);
  gfx->setTextColor(color, BG);
  int16_t cursorX = x;
  int16_t cursorY = y;
  int16_t charW = 6 * size;
  int16_t lineH = 8 * size + 4;
  uint8_t line = 0;

  String word;
  for (size_t i = 0; i <= text.length(); i++) {
    char c = i < text.length() ? text[i] : ' ';
    if (c == ' ' || i == text.length()) {
      int16_t wordW = word.length() * charW;
      if (cursorX != x && cursorX + wordW > x + maxW) {
        line++;
        if (line >= lines) return;
        cursorX = x;
        cursorY += lineH;
      }
      gfx->setCursor(cursorX, cursorY);
      gfx->print(word);
      cursorX += wordW + charW;
      word = "";
    } else {
      word += c;
    }
  }
}

// Return the current visible slice of long text, pausing at the start/end of the scroll.
String marqueeText(const String &text, uint16_t &offset, uint8_t &pauseTicks, uint8_t visibleChars) {
  if (text.length() <= visibleChars) return text;

  uint16_t maxOffset = text.length() - visibleChars;
  if (offset > maxOffset) offset = maxOffset;

  String out;
  for (uint8_t i = 0; i < visibleChars; i++) {
    out += text[offset + i];
  }

  if (pauseTicks > 0) {
    pauseTicks--;
  } else {
    if (offset >= maxOffset) {
      offset = 0;
      pauseTicks = 3;
    } else {
      offset++;
    }
  }

  return out;
}

// Draw or advance the song title and artist text without redrawing unchanged text.
void drawTextFields(bool reset, bool advance) {
  if (reset) {
    titleScroll = 0;
    artistScroll = 0;
    titlePauseTicks = 3;
    artistPauseTicks = 3;
    lastVisibleTitle = "";
    lastVisibleArtist = "";
  }

  uint16_t savedTitleScroll = titleScroll;
  uint8_t savedTitlePause = titlePauseTicks;
  uint16_t savedArtistScroll = artistScroll;
  uint8_t savedArtistPause = artistPauseTicks;

  String visibleTitle = marqueeText(currentTitle, titleScroll, titlePauseTicks, 17);
  if (!advance) {
    titleScroll = savedTitleScroll;
    titlePauseTicks = savedTitlePause;
  }
  if (reset || visibleTitle != lastVisibleTitle) {
    gfx->fillRect(TEXT_X, 254, TEXT_W, 20, BG);
    lastVisibleTitle = visibleTitle;
    gfx->setTextSize(2);
    gfx->setTextColor(FG, BG);
    gfx->setCursor(TEXT_X, 254);
    gfx->print(visibleTitle);
  }

  String visibleArtist = marqueeText(currentArtist, artistScroll, artistPauseTicks, 17);
  if (!advance) {
    artistScroll = savedArtistScroll;
    artistPauseTicks = savedArtistPause;
  }
  if (reset || visibleArtist != lastVisibleArtist) {
    gfx->fillRect(TEXT_X, 276, 104, 12, BG);
    lastVisibleArtist = visibleArtist;
    gfx->setTextSize(1);
    gfx->setTextColor(MUTED, BG);
    gfx->setCursor(TEXT_X, 278);
    gfx->print(visibleArtist);
  }
}

// True when either metadata line is longer than the available on-screen characters.
bool textNeedsScroll() {
  return currentTitle.length() > 17 || currentArtist.length() > 17;
}

// Draw the volume bar and play/pause indicator, only refreshing changed controls.
void drawControls(bool force) {
  int volX = 128;
  int volY = 279;
  int volW = 62;
  int iconX = 204;
  int iconY = 274;

  if (force || !controlsDrawn || currentVolumePercent != lastDrawnVolumePercent) {
    gfx->fillRect(volX, volY + 2, volW, 5, PANEL);
    int volFillW = map(constrain(currentVolumePercent, 0, 100), 0, 100, 0, volW);
    gfx->fillRect(volX, volY + 2, volFillW, 5, MUTED);
    lastDrawnVolumePercent = currentVolumePercent;
  }

  if (force || !controlsDrawn || isPlaying != lastDrawnPlaying) {
    gfx->fillRect(iconX - 1, iconY - 1, 20, 20, BG);
    if (isPlaying) {
      gfx->fillRoundRect(iconX, iconY, 5, 18, 1, FG);
      gfx->fillRoundRect(iconX + 10, iconY, 5, 18, 1, FG);
    } else {
      gfx->fillTriangle(iconX, iconY, iconX, iconY + 18, iconX + 16, iconY + 9, FG);
    }
    lastDrawnPlaying = isPlaying;
  }
  controlsDrawn = true;
}

// Draw the bottom playback progress bar and elapsed/total time labels.
void drawProgress(int progressMs, int durationMs, bool force = false) {
  int barX = 16;
  int barY = 302;
  int barW = 208;
  int barH = 8;

  int fillW = durationMs > 0 ? map(progressMs, 0, durationMs, 0, barW) : 0;
  fillW = constrain(fillW, 0, barW);
  if (force || fillW != lastDrawnProgressFill) {
    gfx->fillRoundRect(barX, barY, barW, barH, 4, PANEL);
    gfx->fillRoundRect(barX, barY, fillW, barH, 4, ACCENT);
    lastDrawnProgressFill = fillW;
  }

  auto fmt = [](int ms) {
    int total = max(0, ms / 1000);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return String(buf);
  };

  static int lastElapsedSec = -1;
  static int lastDurationSec = -1;
  int elapsedSec = progressMs / 1000;
  int durationSec = durationMs / 1000;
  if (force || elapsedSec != lastElapsedSec) {
    gfx->fillRect(8, 312, 42, 8, BG);
    gfx->setTextSize(1);
    gfx->setTextColor(MUTED, BG);
    gfx->setCursor(8, 312);
    gfx->print(fmt(progressMs));
    lastElapsedSec = elapsedSec;
  }

  if (force || durationSec != lastDurationSec) {
    gfx->fillRect(194, 312, 38, 8, BG);
    gfx->setTextSize(1);
    gfx->setTextColor(MUTED, BG);
    gfx->setCursor(204, 312);
    gfx->print(fmt(durationMs));
    lastDurationSec = durationSec;
  }
}

// JPEGDEC callback: either scan decoded pixels for green failure or draw them scaled into the album box.
int jpegDrawCallback(JPEGDRAW *pDraw) {
  uint16_t *pixels = (uint16_t *)pDraw->pPixels;
  for (int16_t y = 0; y < pDraw->iHeight; y++) {
    int16_t srcY = pDraw->y + y;
    if (albumScanOnly) {
      for (int16_t x = 0; x < pDraw->iWidth; x++) {
        uint16_t raw = pixels[y * pDraw->iWidth + x];
        albumSampleCount++;
        if (isGreenDominant565(raw)) albumGreenDominantCount++;
      }
      continue;
    }

    int16_t dstY = ART_Y + ((int32_t)srcY * ART_SIZE) / jpegSourceH;
    if (dstY < ART_Y || dstY >= ART_Y + ART_SIZE) continue;

    for (int16_t x = 0; x < pDraw->iWidth; x++) {
      int16_t srcX = pDraw->x + x;
      int16_t dstX = ART_X + ((int32_t)srcX * ART_SIZE) / jpegSourceW;
      if (dstX < ART_X || dstX >= ART_X + ART_SIZE) continue;
      int16_t nextDstX = ART_X + ((int32_t)(srcX + 1) * ART_SIZE) / jpegSourceW;
      int16_t nextDstY = ART_Y + ((int32_t)(srcY + 1) * ART_SIZE) / jpegSourceH;
      if (nextDstX <= dstX) nextDstX = dstX + 1;
      if (nextDstY <= dstY) nextDstY = dstY + 1;
      if (nextDstX > ART_X + ART_SIZE) nextDstX = ART_X + ART_SIZE;
      if (nextDstY > ART_Y + ART_SIZE) nextDstY = ART_Y + ART_SIZE;

      uint16_t raw = pixels[y * pDraw->iWidth + x];
      uint16_t color = albumEnhanceColors ? enhanceAlbumColor(raw) : raw;
      for (int16_t yy = dstY; yy < nextDstY; yy++) {
        for (int16_t xx = dstX; xx < nextDstX; xx++) {
          if (insideRoundedArt(xx, yy)) gfx->drawPixel(xx, yy, color);
        }
      }
    }
  }
  return 1;
}

// Decode-scan the downloaded JPEG and reject images that decode as almost entirely green.
bool albumArtIsSolidGreen(const char *label) {
  if (!imageBuffer || imageBufferLen == 0) return false;
  if (!jpeg.openRAM(imageBuffer, imageBufferLen, jpegDrawCallback)) {
    Serial.printf("%s album art JPEG open failed for \"%s\" during green check.\n", label, currentTitle.c_str());
    return false;
  }

  jpegScaleDiv = jpeg.getWidth() > 400 ? 2 : 1;
  jpegSourceW = jpeg.getWidth() / jpegScaleDiv;
  jpegSourceH = jpeg.getHeight() / jpegScaleDiv;
  albumScanOnly = true;
  albumSampleCount = 0;
  albumGreenDominantCount = 0;
  jpeg.decode(0, 0, jpegScaleDiv == 2 ? JPEG_SCALE_HALF : 0);
  albumScanOnly = false;
  jpeg.close();

  if (albumSampleCount < 1000) return false;
  uint32_t greenPct = albumGreenDominantCount * 100UL / albumSampleCount;
  if (greenPct >= 90) {
    Serial.printf("Rejected %s album art for \"%s\": decoded as solid green (%u%% green samples).\n",
                  label, currentTitle.c_str(), (unsigned)greenPct);
    return true;
  }
  return false;
}

// Download one album-art JPEG into PSRAM/RAM, with support for fixed-length or streamed responses.
bool downloadAlbumArt(const String &url, const char *label, uint8_t attempt) {
  if (imageBuffer) {
    free(imageBuffer);
    imageBuffer = nullptr;
    imageBufferLen = 0;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(client, url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("%s album art failed for \"%s\" attempt %u: HTTP %d\n", label, currentTitle.c_str(), attempt, code);
    http.end();
    return false;
  }

  int contentLength = http.getSize();
  bool unknownLength = contentLength <= 0;
  if (contentLength > (int)MAX_ALBUM_ART_BYTES) {
    Serial.printf("%s album art failed for \"%s\" attempt %u: too large (%d bytes)\n",
                  label, currentTitle.c_str(), attempt, contentLength);
    http.end();
    return false;
  }
  imageBufferLen = unknownLength ? UNKNOWN_ALBUM_ART_BYTES : (size_t)contentLength;
  if (unknownLength) {
    Serial.printf("%s album art for \"%s\" attempt %u has no Content-Length; reading streamed response.\n",
                  label, currentTitle.c_str(), attempt);
  }

  imageBuffer = (uint8_t *)ps_malloc(imageBufferLen);
  if (!imageBuffer) imageBuffer = (uint8_t *)malloc(imageBufferLen);
  if (!imageBuffer) {
    Serial.printf("%s album art failed for \"%s\" attempt %u: not enough memory for %u bytes\n",
                  label, currentTitle.c_str(), attempt, (unsigned)imageBufferLen);
    printMemoryStatus("Album art allocation failed");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  stream->setTimeout(10000);
  size_t offset = 0;
  uint32_t lastReadAt = millis();
  while (http.connected() && offset < imageBufferLen) {
    int available = stream->available();
    if (available) {
      int readLen = stream->readBytes(imageBuffer + offset, min((size_t)available, imageBufferLen - offset));
      offset += readLen;
      lastReadAt = millis();
    } else if (millis() - lastReadAt > 10000) {
      break;
    }
    delay(1);
  }
  http.end();

  if (unknownLength) {
    if (offset == imageBufferLen) {
      Serial.printf("%s album art failed for \"%s\" attempt %u: streamed image exceeded %u bytes\n",
                    label, currentTitle.c_str(), attempt, (unsigned)imageBufferLen);
      free(imageBuffer);
      imageBuffer = nullptr;
      imageBufferLen = 0;
      return false;
    }
    imageBufferLen = offset;
  } else if (offset != imageBufferLen) {
    Serial.printf("%s album art truncated for \"%s\" attempt %u: got %u of %u bytes\n",
                  label, currentTitle.c_str(), attempt, (unsigned)offset, (unsigned)imageBufferLen);
    free(imageBuffer);
    imageBuffer = nullptr;
    imageBufferLen = 0;
    return false;
  }

  if (imageBufferLen < 4 || imageBuffer[0] != 0xFF || imageBuffer[1] != 0xD8) {
    Serial.printf("%s album art for \"%s\" attempt %u is not a JPEG.\n", label, currentTitle.c_str(), attempt);
    free(imageBuffer);
    imageBuffer = nullptr;
    imageBufferLen = 0;
    return false;
  }

  Serial.printf("%s album art downloaded for \"%s\" attempt %u: %u bytes\n",
                label, currentTitle.c_str(), attempt, (unsigned)imageBufferLen);
  return true;
}

// Try one Spotify album-art URL twice, then reject it if the decoded image is solid green.
bool tryAlbumArtUrl(const String &url, const char *label) {
  if (!url.length()) {
    Serial.printf("No %s album art URL for \"%s\".\n", label, currentTitle.c_str());
    return false;
  }
  Serial.printf("Trying %s album art for \"%s\"\n", label, currentTitle.c_str());
  for (uint8_t attempt = 1; attempt <= 2; attempt++) {
    if (downloadAlbumArt(url, label, attempt)) {
      if (!albumArtIsSolidGreen(label)) return true;
      free(imageBuffer);
      imageBuffer = nullptr;
      imageBufferLen = 0;
      return false;
    }
    delay(250);
  }
  return false;
}

// Pick the largest useful Spotify album-art image, falling back to a medium image if needed.
bool downloadBestAlbumArt(JsonArray images) {
  String largeUrl;
  String backupUrl;
  int largeWidth = -1;
  int backupWidth = -1;

  for (JsonObject image : images) {
    String url = image["url"].as<String>();
    int width = image["width"] | 0;
    if (!url.length()) continue;

    if (width > largeWidth) {
      backupUrl = largeUrl;
      backupWidth = largeWidth;
      largeUrl = url;
      largeWidth = width;
    } else if (width > backupWidth) {
      backupUrl = url;
      backupWidth = width;
    }
  }

  if (tryAlbumArtUrl(largeUrl, "large")) return true;
  if (backupWidth >= 250 && tryAlbumArtUrl(backupUrl, "medium")) return true;

  Serial.printf("No usable album art for \"%s\".\n", currentTitle.c_str());
  return false;
}

// Draw the current album art, or an "Art unavailable" placeholder if no JPEG is available.
void drawAlbumArt() {
  gfx->fillRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, BG);
  gfx->fillRoundRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, 8, PANEL);
  if (!imageBuffer || imageBufferLen == 0) {
    gfx->setTextSize(1);
    gfx->setTextColor(MUTED, PANEL);
    gfx->setCursor(ART_X + 60, ART_Y + 99);
    gfx->print("Art unavailable");
    return;
  }

  if (jpeg.openRAM(imageBuffer, imageBufferLen, jpegDrawCallback)) {
    jpegScaleDiv = jpeg.getWidth() > 400 ? 2 : 1;
    jpegSourceW = jpeg.getWidth() / jpegScaleDiv;
    jpegSourceH = jpeg.getHeight() / jpegScaleDiv;
    albumEnhanceColors = true;
    albumScanOnly = false;
    Serial.printf("JPEG decode for \"%s\": %dx%d scaleDiv=%u\n", currentTitle.c_str(), jpeg.getWidth(), jpeg.getHeight(), jpegScaleDiv);
    jpeg.decode(0, 0, jpegScaleDiv == 2 ? JPEG_SCALE_HALF : 0);
    jpeg.close();
  } else {
    Serial.printf("JPEG open failed for \"%s\".\n", currentTitle.c_str());
  }

  static constexpr int16_t r = 8;
  for (int16_t y = 0; y < r; y++) {
    for (int16_t x = 0; x < r; x++) {
      int16_t dx = x - r;
      int16_t dy = y - r;
      if (dx * dx + dy * dy > r * r) gfx->drawPixel(ART_X + x, ART_Y + y, BG);

      dx = x + 1;
      dy = y - r;
      if (dx * dx + dy * dy > r * r) gfx->drawPixel(ART_X + ART_SIZE - r + x, ART_Y + y, BG);

      dx = x - r;
      dy = y + 1;
      if (dx * dx + dy * dy > r * r) gfx->drawPixel(ART_X + x, ART_Y + ART_SIZE - r + y, BG);

      dx = x + 1;
      dy = y + 1;
      if (dx * dx + dy * dy > r * r) gfx->drawPixel(ART_X + ART_SIZE - r + x, ART_Y + ART_SIZE - r + y, BG);
    }
  }
}

// Draw the full now-playing screen on track/context changes and update the progress bar every tick.
void drawNowPlaying(bool newTrack) {
  if (newTrack) {
    lastIdleMessage = "";
    drawHeader(currentContextName.c_str());
    gfx->fillRect(0, ART_Y, SCREEN_W, 282 - ART_Y, BG);
    lastDrawnProgressFill = -1;
    lastDrawnVolumePercent = -1;
    controlsDrawn = false;
    drawAlbumArt();
    drawTextFields(true, false);
    drawControls(true);
  }
  int progress = lastProgressMs;
  if (isPlaying) progress += millis() - lastProgressSyncMs;
  progress = constrain(progress, 0, currentDurationMs);
  drawProgress(progress, currentDurationMs, newTrack);
}

// Draw a static idle/status screen, skipping redraws when the same message is already visible.
void drawIdle(const char *message) {
  String messageText = message;
  if (lastIdleMessage == messageText) return;
  lastIdleMessage = messageText;

  gfx->fillScreen(BG);
  drawHeader("Spotify");
  gfx->setTextSize(2);
  gfx->setTextColor(FG, BG);
  gfx->setCursor(18, 92);
  gfx->print(message);
}

// Resolve the Spotify context name, such as a playlist name, with simple caching/rate-limit handling.
String fetchContextName(const String &href, const String &type, const String &uri) {
  if (uri.length() && uri == cachedContextUri && cachedContextName.length()) {
    return cachedContextName;
  }

  if (contextRetryAfterMs && millis() < contextRetryAfterMs) {
    return cachedContextName.length() ? cachedContextName : "Spotify";
  }

  String lookupUrl = href;
  if (type == "playlist" && uri.startsWith("spotify:playlist:")) {
    String playlistId = uri.substring(String("spotify:playlist:").length());
    lookupUrl = "https://api.spotify.com/v1/playlists/" + playlistId + "?fields=name";
  } else if (type == "playlist" && lookupUrl.length()) {
    lookupUrl += "?fields=name";
  }

  if (!lookupUrl.length()) return "Spotify";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, lookupUrl);
  const char *headers[] = {"Retry-After"};
  http.collectHeaders(headers, 1);
  http.addHeader("Authorization", "Bearer " + accessToken);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Context lookup failed for %s: HTTP %d\n%s\n", lookupUrl.c_str(), code, http.getString().c_str());
    if (code == 429) {
      String retryAfter = http.header("Retry-After");
      uint32_t retrySeconds = retryAfter.length() ? retryAfter.toInt() : 300;
      if (retrySeconds == 0) retrySeconds = 300;
      contextRetryAfterMs = millis() + retrySeconds * 1000UL;
      Serial.printf("Context lookup rate-limited. Retrying after %u seconds.\n", (unsigned)retrySeconds);
    }
    http.end();
    return cachedContextName.length() ? cachedContextName : "Spotify";
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("Context JSON parse failed for %s: %s\n", lookupUrl.c_str(), err.c_str());
    return cachedContextName.length() ? cachedContextName : "Spotify";
  }

  String name = doc["name"].as<String>();
  if (!name.length()) name = doc["title"].as<String>();
  if (!name.length()) name = "Spotify";
  cachedContextUri = uri;
  cachedContextName = name;
  contextRetryAfterMs = 0;
  Serial.printf("Context name: %s\n", name.c_str());
  return name;
}

// Poll Spotify for the current track, update local state, and redraw only what changed.
bool fetchCurrentlyPlaying() {
  if (!refreshAccessToken()) {
    drawIdle("Auth failed");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, "https://api.spotify.com/v1/me/player?additional_types=track");
  http.addHeader("Authorization", "Bearer " + accessToken);
  int code = http.GET();

  if (code == 204) {
    http.end();
    updateIdleSleepTimer();
    if (!screenSleeping) {
      drawIdle("Nothing playing");
    }
    lastTrackId = "";
    return true;
  }

  if (code != 200) {
    Serial.printf("Currently playing failed: HTTP %d\n%s\n", code, http.getString().c_str());
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err || doc["item"].isNull()) return false;
  idleSinceMs = 0;

  String trackId = doc["item"]["id"].as<String>();
  String contextType = doc["context"]["type"].as<String>();
  String contextHref = doc["context"]["href"].as<String>();
  String contextUri = doc["context"]["uri"].as<String>();
  String nextContextName = fetchContextName(contextHref, contextType, contextUri);
  currentTitle = doc["item"]["name"].as<String>();
  currentArtist = "";
  JsonArray artists = doc["item"]["artists"].as<JsonArray>();
  for (JsonObject artist : artists) {
    if (currentArtist.length()) currentArtist += ", ";
    currentArtist += artist["name"].as<String>();
  }
  currentDurationMs = doc["item"]["duration_ms"] | 0;
  currentVolumePercent = doc["device"]["volume_percent"] | 0;
  lastProgressMs = doc["progress_ms"] | 0;
  lastProgressSyncMs = millis();
  isPlaying = doc["is_playing"] | false;
  bool wokeScreen = updatePauseSleepTimer();

  bool newTrack = wokeScreen || trackId != lastTrackId || nextContextName != currentContextName;
  currentContextName = nextContextName;
  if (newTrack) {
    lastTrackId = trackId;
    downloadBestAlbumArt(doc["item"]["album"]["images"].as<JsonArray>());
  }

  if (!screenSleeping) {
    drawNowPlaying(newTrack);
    if (!newTrack) {
      drawControls(false);
    }
  }
  return true;
}

// Arduino entry point: initialize serial, preferences, display, backlight, Wi-Fi, and startup UI.
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Spotify TFT booting...");
  printMemoryStatus("Boot");
  prefs.begin("spotify", false);
  refreshToken = prefs.getString("refresh", SPOTIFY_REFRESH_TOKEN);
  if (TFT_RST != GFX_NOT_DEFINED) {
    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, LOW);
    delay(50);
    digitalWrite(TFT_RST, HIGH);
    delay(150);
  }
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  Serial.println("Backlight pin set.");

  if (!gfx->begin(2000000)) {
    Serial.println("Display init failed");
  } else {
    Serial.println("Display init OK");
  }
  gfx->invertDisplay(true);
  gfx->fillScreen(0xF800);
  delay(350);
  gfx->fillScreen(0x07E0);
  delay(350);
  gfx->fillScreen(0x001F);
  delay(350);
  gfx->fillScreen(BG);
  gfx->setTextColor(FG, BG);
  gfx->setTextSize(2);
  gfx->setCursor(20, 140);
  gfx->print("Display OK");
  delay(800);
  Serial.println("Connecting to Wi-Fi...");
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    drawIdle("Starting...");
  }
}

// Arduino main loop: reconnect Wi-Fi, poll Spotify, animate progress, and scroll long text.
void loop() {
  static uint32_t lastFetchMs = 0;
  static uint32_t lastProgressDrawMs = 0;
  static uint32_t lastTextDrawMs = 0;
  static uint32_t lastWiFiAttemptMs = 0;

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiAttemptMs > 30000) {
      lastWiFiAttemptMs = millis();
      connectWiFi();
    }
    return;
  }

  if (millis() - lastFetchMs > 5000) {
    fetchCurrentlyPlaying();
    lastFetchMs = millis();
  }

  if (!screenSleeping && lastTrackId.length() && millis() - lastProgressDrawMs > 200) {
    drawNowPlaying(false);
    lastProgressDrawMs = millis();
  }

  if (!screenSleeping && lastTrackId.length() && textNeedsScroll() && millis() - lastTextDrawMs > 450) {
    drawTextFields(false, true);
    lastTextDrawMs = millis();
  }
}
