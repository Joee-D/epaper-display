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

String buildServerUrl(const DeviceConfig &cfg) {
  String host = cfg.serverIp;
  host.trim();
  // Tolerate a pasted URL: drop scheme, port, and path if present.
  int scheme = host.indexOf("://");
  if (scheme >= 0) host = host.substring(scheme + 3);
  int slash = host.indexOf('/');
  if (slash >= 0) host = host.substring(0, slash);
  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  if (host.length() == 0) return "";
  return "http://" + host + ":" + String(SERVER_PORT) + SERVER_PATH;
}

void configLoad(DeviceConfig &cfg) {
  Preferences prefs;
  prefs.begin(NS, /*readOnly=*/true);
  cfg.serverIp    = prefs.getString("server_ip", "");
  cfg.serverIp.trim();
  cfg.intervalMin = prefs.getUInt("interval_min", DEFAULT_INTERVAL_MIN);
  cfg.rotate180   = prefs.getBool("rotate180", false);
  cfg.keepWifi    = prefs.getBool("keepwifi", true);
  prefs.end();

  cfg.intervalMin = normalizedInterval(cfg.intervalMin);
}

void configSave(const DeviceConfig &cfg) {
  Preferences prefs;
  prefs.begin(NS, /*readOnly=*/false);
  String serverIp = cfg.serverIp;
  serverIp.trim();
  prefs.putString("server_ip", serverIp);
  prefs.putUInt("interval_min", normalizedInterval(cfg.intervalMin));
  prefs.putBool("rotate180", cfg.rotate180);
  prefs.putBool("keepwifi", cfg.keepWifi);
  prefs.end();
}

void configFactoryReset() {
  Preferences prefs;
  prefs.begin(NS, false);
  prefs.clear();
  prefs.end();

  // Ensure the WiFi stack is initialized before erasing credentials, as this
  // is also called early in setup() on cold boots.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true); // erase stored WiFi creds too
}
