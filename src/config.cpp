#include "config.h"
#include <Preferences.h>
#include <WiFi.h>

static const char *NS = "epaper-display";

static const uint32_t DEFAULT_INTERVAL_MIN = 15;

static uint32_t normalizedInterval(uint32_t intervalMin) {
  if (intervalMin < MIN_REFRESH_INTERVAL_MIN ||
      intervalMin > MAX_REFRESH_INTERVAL_MIN) {
    return DEFAULT_INTERVAL_MIN;
  }
  return intervalMin;
}

void configLoad(DeviceConfig &cfg) {
  Preferences prefs;
  prefs.begin(NS, /*readOnly=*/true);
  cfg.serverUrl   = prefs.getString("server_url", "");
  cfg.serverUrl.trim();
  cfg.intervalMin = prefs.getUInt("interval_min", DEFAULT_INTERVAL_MIN);
  cfg.rotate180   = prefs.getBool("rotate180", false);
  prefs.end();

  cfg.intervalMin = normalizedInterval(cfg.intervalMin);
}

void configSave(const DeviceConfig &cfg) {
  Preferences prefs;
  prefs.begin(NS, /*readOnly=*/false);
  String serverUrl = cfg.serverUrl;
  serverUrl.trim();
  prefs.putString("server_url", serverUrl);
  prefs.putUInt("interval_min", normalizedInterval(cfg.intervalMin));
  prefs.putBool("rotate180", cfg.rotate180);
  prefs.end();
}

void configFactoryReset() {
  Preferences prefs;
  prefs.begin(NS, false);
  prefs.clear();
  prefs.end();

  WiFi.disconnect(true, true); // erase stored WiFi creds too
}
