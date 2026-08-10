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
// built-in SSID/password fields. Only the server IP is entered here; the
// scheme, port, and endpoint path are filled in automatically by
// buildServerUrl().
WiFiManagerParameter paramServerIp("server", "Image server IP", "", 45);
WiFiManagerParameter paramInterval("interval", "Refresh interval (minutes)", "15", 6);
WiFiManagerParameter paramRotate180("rotate180", "Rotate display 180 degrees (0/1)", "0", 1);
WiFiManagerParameter paramKeepWifi("keepwifi", "Keep WiFi across sleep for USB power (1=USB, 0=battery)", "1", 1);

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
  cfg.serverIp    = paramServerIp.getValue();
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

  paramServerIp.setValue(cfg.serverIp.c_str(), 45);
  paramInterval.setValue(String(cfg.intervalMin).c_str(), 6);
  paramRotate180.setValue(cfg.rotate180 ? "1" : "0", 1);
  paramKeepWifi.setValue(cfg.keepWifi ? "1" : "0", 1);
  wm.addParameter(&paramServerIp);
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
  uint64_t us = (uint64_t)cfg.intervalMin * 60ULL * 1000000ULL;
  esp_sleep_enable_timer_wakeup(us);

  if (cfg.keepWifi) {
    // Mains powered: light sleep and keep the WiFi association (modem sleep
    // stays on) so the next cycle skips the reconnect. If the association is
    // ever lost, runCycle() detects it and reconnects.
    WiFi.setSleep(true);
    esp_light_sleep_start();
    return;  // resumes after the interval; runCycle() runs again
  }

  WiFi.mode(WIFI_OFF);
  esp_deep_sleep_start();
}

void runCycle() {
  displayInit(cfg);

  bool forcePortal = configButtonHeld();

  // Battery/deep-sleep boots (or a dropped keepWiFi association) arrive with
  // WiFi down and reconnect here. In keepWifi mode the association survives
  // light sleep, so WiFi.status() is already WL_CONNECTED and we skip it.
  if (forcePortal || cfg.serverIp.length() == 0) {
    runProvisioningPortal(true);
  } else if (WiFi.status() != WL_CONNECTED) {
    if (!wm.autoConnect("epaper-display-Setup")) {
      goToSleep(cfg);
      return;
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (cfg.serverIp.length() == 0) {
      displayShowMessage("No server configured",
                          "Hold BOOT button 3s to open setup");
    } else {
      displayFetchAndShow(cfg, deviceId());
    }
  }

  goToSleep(cfg);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  configLoad(cfg);

  // WiFiManager will silently reuse previously-saved WiFi credentials if
  // they exist and connect fast. Never auto-open the captive portal on a
  // failed connect: the portal runs only when explicitly forced (config
  // button) or on first boot (no server configured). A bad network must
  // just sleep and retry next cycle instead of entering setup mode.
  wm.setConnectTimeout(15);
  wm.setEnableConfigPortal(false);
}

void loop() {
  // Deep sleep never returns, so this runs once per wake; light sleep in
  // keepWifi mode returns and loops back into the next cycle.
  runCycle();
}
