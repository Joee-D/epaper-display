#include "display.h"
#include <SPI.h>
#include <HTTPClient.h>
#include <WiFiClient.h>

#include <GxEPD2_BW.h>
#include <gdey/GxEPD2_420_GDEY042T81.h>

static GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> epd(
    GxEPD2_420_GDEY042T81(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

// After this many partial updates, do a full refresh to remove ghosting.
constexpr uint8_t FULL_REFRESH_INTERVAL = 5;
RTC_DATA_ATTR static uint8_t partialRefreshCount = FULL_REFRESH_INTERVAL;

// Content change detection: remember a hash of the last bitmap actually drawn
// so an identical frame (e.g. flat market, market closed) skips the expensive
// e-paper redraw. lastDrawWasBitmap distinguishes "panel shows the bitmap"
// from "panel shows a status message", so a message never gets overwritten
// by a stale-hash skip.
RTC_DATA_ATTR static uint32_t lastImageHash = 0;
RTC_DATA_ATTR static bool lastDrawWasBitmap = false;

static uint32_t fnv1a(const uint8_t *data, size_t len) {
  uint32_t hash = 2166136261u;
  while (len--) {
    hash ^= *data++;
    hash *= 16777619u;
  }
  return hash;
}

static bool needsFullRefresh() {
  return partialRefreshCount >= FULL_REFRESH_INTERVAL;
}

void displayInit(const DeviceConfig &cfg) {
  SPI.begin(PIN_EPD_SCK, /*MISO=*/-1, PIN_EPD_MOSI, PIN_EPD_CS);
  
  bool isFullRefresh = needsFullRefresh();
  
  epd.init(115200, isFullRefresh, /*resetDuration=*/20, /*pulldown_rst_mode=*/false);
  epd.setRotation(cfg.rotate180 ? 2 : 0);
}

void displayShowMessage(const String &line1, const String &line2) {
  lastDrawWasBitmap = false;

  if (needsFullRefresh()) {
    epd.setFullWindow();
  } else {
    epd.setPartialWindow(0, 0, epd.width(), epd.height());
  }

  epd.firstPage();
  do {
    epd.fillScreen(GxEPD_WHITE);
    epd.setTextColor(GxEPD_BLACK);
    epd.setTextSize(2);

    int16_t x1, y1;
    uint16_t w, h;
    epd.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
    epd.setCursor((epd.width() - w) / 2, epd.height() / 2 - 20);
    epd.print(line1);

    epd.setTextSize(1);
    epd.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
    epd.setCursor((epd.width() - w) / 2, epd.height() / 2 + 10);
    epd.print(line2);
  } while (epd.nextPage());
}

bool displayFetchAndShow(const DeviceConfig &cfg, const String &deviceId) {
  String url = buildServerUrl(cfg);
  if (url.length() == 0) {
    Serial.println("Image fetch skipped: no server configured");
    return false;
  }

  // MAC addresses contain ':'; encode the identifier so it remains a valid
  // query value if the server later uses stricter URL parsing.
  String encodedId;
  for (size_t i = 0; i < deviceId.length(); ++i) {
    const char c = deviceId[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encodedId += c;
    } else {
      char escaped[4];
      snprintf(escaped, sizeof(escaped), "%%%02X", static_cast<unsigned char>(c));
      encodedId += escaped;
    }
  }

  url += (url.indexOf('?') >= 0) ? '&' : '?';
  url += "w=" + String(EPD_WIDTH) + "&h=" + String(EPD_HEIGHT) + "&id=" + encodedId;

  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(url)) {
    Serial.println("Image fetch failed: invalid server URL");
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Image fetch failed: HTTP %d\n", code);
    http.end();
    return false;
  }

  int len = http.getSize();
  // Chunked HTTP responses report -1 here. They are valid as long as the
  // stream delivers exactly one framebuffer below.
  if (len >= 0 && len != EPD_IMAGE_BYTES) {
    Serial.printf("Image fetch failed: expected %d bytes, got %d\n",
                  EPD_IMAGE_BYTES, len);
    http.end();
    return false;
  }

  uint8_t *buf = (uint8_t *)malloc(EPD_IMAGE_BYTES);
  if (!buf) {
    Serial.println("Image fetch failed: out of memory");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t received = 0;
  unsigned long start = millis();
  while (received < (size_t)EPD_IMAGE_BYTES && millis() - start < 20000) {
    if (stream->available()) {
      int n = stream->read(buf + received, EPD_IMAGE_BYTES - received);
      if (n > 0) received += n;
    } else {
      delay(5);
    }
  }
  http.end();

  if (received != (size_t)EPD_IMAGE_BYTES) {
    free(buf);
    Serial.printf("Image fetch failed: incomplete download (%u/%d bytes)\n",
                  static_cast<unsigned>(received), EPD_IMAGE_BYTES);
    return false;
  }

  // Identical frame to the last one actually shown: nothing to redraw. This
  // skips the ~2s panel refresh and avoids e-ink wear on every tick.
  uint32_t hash = fnv1a(buf, EPD_IMAGE_BYTES);
  if (lastDrawWasBitmap && lastImageHash != 0 && hash == lastImageHash) {
    free(buf);
    Serial.println("Image unchanged; skipping redraw");
    return true;
  }

  if (needsFullRefresh()) {
    epd.setFullWindow();
  } else {
    epd.setPartialWindow(0, 0, EPD_WIDTH, EPD_HEIGHT);
  }

  epd.firstPage();
  do {
    epd.fillScreen(GxEPD_WHITE);
    epd.drawBitmap(0, 0, buf, EPD_WIDTH, EPD_HEIGHT, GxEPD_BLACK);
  } while (epd.nextPage());

  lastImageHash = hash;
  lastDrawWasBitmap = true;
  free(buf);

  if (needsFullRefresh()) {
    partialRefreshCount = 0;
  } else {
    ++partialRefreshCount;
  }

  return true;
}

void displayHibernate() {
  epd.hibernate();
}
