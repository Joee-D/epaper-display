#include "display.h"
#include <SPI.h>

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

bool displayShowBitmap(const uint8_t *bitmap) {
  if (bitmap == nullptr) return false;

  // Identical frame to the last one actually shown: nothing to redraw. This
  // skips the ~2s panel refresh and avoids e-ink wear on every tick.
  uint32_t hash = fnv1a(bitmap, EPD_IMAGE_BYTES);
  if (lastDrawWasBitmap && lastImageHash != 0 && hash == lastImageHash) {
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
    epd.drawBitmap(0, 0, bitmap, EPD_WIDTH, EPD_HEIGHT, GxEPD_BLACK);
  } while (epd.nextPage());

  lastImageHash = hash;
  lastDrawWasBitmap = true;

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
