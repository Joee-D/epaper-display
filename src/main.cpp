// epaper-display
// ---------------------------------------------------------------------------
// A self-contained ESP32 + e-ink firmware inspired by
// https://github.com/datascale-ai/inksight :
//   1. First boot (or held config button) -> WiFi captive-portal provisioning
//      page (SSID/password + refresh interval).
//   2. Every wake-up: connect WiFi, pick the market session that is open right
//      now, fetch its quotes, draw the chart on the device, push it to the
//      panel, then deep-sleep until the next cycle.
//
// No backend is involved: quotes come from Yahoo Finance and the chart is
// rendered locally (see src/market.* and src/chart.*).
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_sleep.h>

#include "chart.h"
#include "config.h"
#include "display.h"
#include "market.h"

DeviceConfig cfg;
WiFiManager wm;

// Custom fields shown on the WiFiManager config portal, alongside the
// built-in SSID/password fields.
WiFiManagerParameter paramInterval("interval", "Refresh interval (minutes)", "15", 6);
WiFiManagerParameter paramRotate180("rotate180", "Rotate display 180 degrees (0/1)", "0", 1);
WiFiManagerParameter paramKeepWifi("keepwifi", "Keep WiFi on between refreshes for USB power (1=USB, 0=battery)", "1", 1);

bool configButtonHeld() {
  pinMode(PIN_CONFIG_BUTTON, INPUT_PULLUP);
  if (digitalRead(PIN_CONFIG_BUTTON) != LOW) return false;

  // debounce / require a real hold, not a bump
  uint32_t start = millis();
  while (digitalRead(PIN_CONFIG_BUTTON) == LOW) {
    if (millis() - start > 3000) return true;
    delay(10);
  }
  return false;
}

void saveConfigCallback() {
  cfg.intervalMin = String(paramInterval.getValue()).toInt();
  if (cfg.intervalMin < MIN_REFRESH_INTERVAL_MIN ||
      cfg.intervalMin > MAX_REFRESH_INTERVAL_MIN) {
    cfg.intervalMin = 15;
  }
  cfg.rotate180 = String(paramRotate180.getValue()).toInt() != 0;
  cfg.keepWifi  = String(paramKeepWifi.getValue()).toInt() != 0;
  configSave(cfg);
}

void runProvisioningPortal(bool forced) {
  displayShowMessage("Setup mode",
                      "Join WiFi \"epaper-display-Setup\" then open 192.168.4.1");

  paramInterval.setValue(String(cfg.intervalMin).c_str(), 6);
  paramRotate180.setValue(cfg.rotate180 ? "1" : "0", 1);
  paramKeepWifi.setValue(cfg.keepWifi ? "1" : "0", 1);
  wm.addParameter(&paramInterval);
  wm.addParameter(&paramRotate180);
  wm.addParameter(&paramKeepWifi);
  wm.setSaveParamsCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(300); // give up and retry later if nobody shows up

  bool ok;
  if (forced) {
    ok = wm.startConfigPortal("epaper-display-Setup");
  } else {
    ok = wm.autoConnect("epaper-display-Setup");
  }

  if (!ok) {
    displayShowMessage("Setup timed out", "Will retry after deep sleep");
  }
}

void goToSleep(const DeviceConfig &cfg) {
  displayHibernate();

  if (cfg.keepWifi) {
    // USB/mains powered: no power constraint, so keep the WiFi association
    // alive by staying awake (modem sleep) between refreshes instead of
    // sleeping. Light sleep here would power down the radio and silently drop
    // the connection (ESP-IDF docs), after which the device could never fetch
    // again — so a plain delay() is both simpler and reliable.
    uint64_t ms = (uint64_t)cfg.intervalMin * 60ULL * 1000ULL;
    WiFi.setSleep(true);
    Serial.printf("Keeping WiFi on; next refresh in %u min\n", cfg.intervalMin);
    delay((uint32_t)ms);
    return;  // loop() runs runCycle() again with WiFi still connected
  }

  uint64_t us = (uint64_t)cfg.intervalMin * 60ULL * 1000000ULL;
  esp_sleep_enable_timer_wakeup(us);
  WiFi.mode(WIFI_OFF);
  esp_deep_sleep_start();
}

// Fetches the active market's quotes from Yahoo Finance, draws the chart on the
// device and pushes it to the panel. Transient failures leave the last chart on
// the panel instead of replacing it with an error page.
void displayLocalChart() {
  if (!marketSyncTime()) {
    Serial.println("Local chart skipped: clock not set");
    displayShowMessage("Clock not set", "NTP unreachable; retrying next cycle");
    return;
  }

  struct tm nowEt;
  if (!marketNow(nowEt)) {
    Serial.println("Local chart skipped: clock not set");
    return;
  }

  const MarketSegment *segment = marketActiveSegment(nowEt);
  if (segment == nullptr) {
    Serial.println("Local chart skipped: every market is closed");
    return;
  }
  Serial.printf("Local chart: %s (%s) at %02d:%02d ET\n", segment->name,
                segment->symbol, nowEt.tm_hour, nowEt.tm_min);

  MarketSeries series;
  if (!marketFetchSeries(*segment, series)) return;

  const float pct =
      (series.price - series.prevClose) / series.prevClose * 100.0f;
  Serial.printf("Local chart: %s %+.2f%% (%d points)\n", segment->displayName,
                pct, series.count);

  chartDisplay(*segment, series);
}

void runCycle() {
  displayInit(cfg);

  bool forcePortal = configButtonHeld();

  // Battery/deep-sleep boots (or a dropped keepWiFi association) arrive with
  // WiFi down and reconnect here. In keepWifi mode we stay awake between
  // refreshes, so WiFi.status() is already WL_CONNECTED and we skip it.
  if (forcePortal || !cfg.provisioned) {
    runProvisioningPortal(true);
  } else if (WiFi.status() != WL_CONNECTED) {
    if (!wm.autoConnect("epaper-display-Setup")) {
      goToSleep(cfg);
      return;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    displayLocalChart();
  }

  goToSleep(cfg);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  configLoad(cfg);

  // The saved configuration survives power cycles; the provisioning portal is
  // reached by holding the BOOT button (see runCycle()). WiFiManager likewise
  // reuses the stored WiFi credentials. Never auto-open the captive portal on
  // a failed connect: a bad network must just sleep and retry next cycle
  // instead of entering setup mode.
  wm.setConnectTimeout(15);
  wm.setEnableConfigPortal(false);
}

void loop() {
  // Deep sleep never returns, so this runs once per wake; keepWifi mode
  // stays awake between refreshes (delay in goToSleep) and loops back into
  // the next cycle with the WiFi association intact.
  runCycle();
}
