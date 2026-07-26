#include "config.h"
#include <Preferences.h>
#include <WiFi.h>

static const char *NS = "epaper-display";

static const char *DEFAULT_SERVER_URL = "http://192.168.1.100:5000/epaper-display/image";
static const uint32_t DEFAULT_INTERVAL_MIN = 15;

void configLoad(DeviceConfig &cfg) {
  Preferences prefs;
  prefs.begin(NS, /*readOnly=*/true);
  cfg.serverUrl   = prefs.getString("server_url", DEFAULT_SERVER_URL);
  cfg.intervalMin = prefs.getUInt("interval_min", DEFAULT_INTERVAL_MIN);
  cfg.rotate180   = prefs.getBool("rotate180", false);
  prefs.end();

  if (cfg.intervalMin == 0) cfg.intervalMin = DEFAULT_INTERVAL_MIN;
}

void configSave(const DeviceConfig &cfg) {
  Preferences prefs;
  prefs.begin(NS, /*readOnly=*/false);
  prefs.putString("server_url", cfg.serverUrl);
  prefs.putUInt("interval_min", cfg.intervalMin);
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
