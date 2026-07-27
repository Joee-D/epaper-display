// epaper-display
// ---------------------------------------------------------------------------
// A minimal, self-contained ESP32 + e-ink firmware inspired by
// https://github.com/datascale-ai/inksight :
//   1. First boot (or held config button) -> WiFi captive-portal provisioning
//      page (SSID/password + your image server URL + refresh interval).
//   2. Every wake-up: connect WiFi, GET a pre-rendered 1bpp bitmap from your
//      server, draw it full-screen, then deep-sleep until the next cycle.
//
// This firmware does NOT try to reproduce InkSight's 24 built-in modes /
// backend / mode plaza — it's a lightweight "pull an image from any server
// on a schedule" client, so you can point it at your own backend, a Home
// Assistant dashboard renderer, a cron job that renders a PNG, etc.
// See server_example/app.py for a minimal Flask server implementing the
// expected image protocol.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_sleep.h>

#include "config.h"
#include "display.h"

DeviceConfig cfg;
WiFiManager wm;

// Custom fields shown on the WiFiManager config portal, alongside the
// built-in SSID/password fields.
WiFiManagerParameter paramServerUrl("server", "Image server URL", "", 200);
WiFiManagerParameter paramInterval("interval", "Refresh interval (minutes)", "15", 6);
WiFiManagerParameter paramRotate180("rotate180", "Rotate display 180 degrees (0/1)", "0", 1);

String deviceId() {
  return WiFi.macAddress();
}

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
  cfg.serverUrl   = paramServerUrl.getValue();
  cfg.intervalMin = String(paramInterval.getValue()).toInt();
  if (cfg.intervalMin < MIN_REFRESH_INTERVAL_MIN ||
      cfg.intervalMin > MAX_REFRESH_INTERVAL_MIN) {
    cfg.intervalMin = 15;
  }
  cfg.rotate180 = String(paramRotate180.getValue()).toInt() != 0;
  configSave(cfg);
}

void runProvisioningPortal(bool forced) {
  displayShowMessage("Setup mode",
                      "Join WiFi \"epaper-display-Setup\" then open 192.168.4.1");

  paramServerUrl.setValue(cfg.serverUrl.c_str(), 200);
  paramInterval.setValue(String(cfg.intervalMin).c_str(), 6);
  paramRotate180.setValue(cfg.rotate180 ? "1" : "0", 1);
  wm.addParameter(&paramServerUrl);
  wm.addParameter(&paramInterval);
  wm.addParameter(&paramRotate180);
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

void goToSleep() {
  displayHibernate();
  WiFi.mode(WIFI_OFF);
  uint64_t us = (uint64_t)cfg.intervalMin * 60ULL * 1000000ULL;
  esp_sleep_enable_timer_wakeup(us);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  configLoad(cfg);
  displayInit(cfg);

  bool forcePortal = configButtonHeld();

  // WiFiManager will silently reuse previously-saved WiFi credentials if
  // they exist and connect fast; it only opens the captive portal if that
  // fails or if the user held the config button.
  wm.setConnectTimeout(15);

  // A device with WiFi credentials but no image endpoint still needs the
  // portal; otherwise it would only show an error until the BOOT button is
  // held on a later wake-up.
  if (forcePortal || cfg.serverUrl.length() == 0) {
    runProvisioningPortal(true);
  } else {
    if (!wm.autoConnect("epaper-display-Setup")) {
      displayShowMessage("WiFi connect failed", "Retrying after deep sleep");
      goToSleep();
      return;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (cfg.serverUrl.length() == 0) {
      displayShowMessage("No server configured",
                          "Hold BOOT button 3s to open setup");
    } else {
      displayFetchAndShow(cfg, deviceId());
    }
  }

  goToSleep();
}

void loop() {
  // never reached — everything happens once per wake cycle in setup()
}
