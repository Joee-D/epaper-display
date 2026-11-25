// epaper-display
// ---------------------------------------------------------------------------
// A minimal ESP32 + e-ink firmware inspired by
// https://github.com/datascale-ai/inksight :
//   1. Wi-Fi and refresh settings live in firmware_config.h.
//   2. Each cycle: sync time, fetch the active Yahoo chart, render it locally,
//      then wait for the next refresh.
//
// No image server is required.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <WiFi.h>

#include "chart.h"
#include "config.h"
#include "display.h"
#include "firmware_config.h"
#include "market.h"

DeviceConfig cfg;

bool connectWifi(const DeviceConfig &cfg) {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (cfg.wifiSsid.length() == 0) {
    Serial.println("WiFi skipped: SSID is empty");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str());
  Serial.printf("Connecting WiFi: %s\n", cfg.wifiSsid.c_str());

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < EPAPER_WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("WiFi connection timed out");
  return false;
}

void waitForNextRefresh(const DeviceConfig &cfg) {
  displayHibernate();

  // The device is mains-powered, so keep the WiFi association alive and only
  // use modem sleep to reduce radio power between refreshes.
  WiFi.setSleep(true);
  Serial.printf("Next refresh in %u min\n", cfg.intervalMin);
  delay((uint32_t)cfg.intervalMin * 60U * 1000U);
}

void displayLocalChart() {
  if (!marketSyncTime()) {
    displayShowMessage("Clock not set", "Check internet access");
    return;
  }

  const time_t now = time(nullptr);
  struct tm marketNow;
  const MarketSegment *segment = marketActiveSegment(now, marketNow);
  if (segment == nullptr) {
    Serial.println("No market is open");
    return;
  }

  Serial.printf("%s market time: %02d:%02d\n", segment->name, marketNow.tm_hour,
                marketNow.tm_min);
  MarketSeries series;
  if (!marketFetchSeries(*segment, now, series)) return;

  const float pct =
      (series.price - series.prevClose) / series.prevClose * 100.0f;
  Serial.printf("%s: %.2f (%+.2f%%), %d points\n", segment->displayName,
                series.price, pct, series.count);
  chartDisplay(*segment, series);
}

void runCycle() {
  displayInit();

  if (!connectWifi(cfg)) {
    displayShowMessage("WiFi connection failed",
                        "Check src/firmware_config.h");
    waitForNextRefresh(cfg);
    return;
  }

  displayLocalChart();

  waitForNextRefresh(cfg);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  configLoad(cfg);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
}

void loop() {
  // The board stays powered, so loop back into the next refresh cycle while
  // the WiFi association remains alive.
  runCycle();
}
