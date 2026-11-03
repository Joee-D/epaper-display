#pragma once
#include <Arduino.h>

// Persisted device configuration (stored in NVS via Preferences).
struct DeviceConfig {
  String   serverIp;      // image server host, e.g. "192.168.1.50"
  uint32_t intervalMin;   // how often to wake up + fetch, in minutes
  bool     rotate180;     // flip the image 180 degrees if mounted upside down
  bool     keepWifi;      // mains-powered: stay awake between refreshes so the WiFi
                          // association survives and reconnects are skipped
  bool     provisioned;   // false until the setup portal has been saved once, so
                          // a fresh device opens it and later boots do not
};

// Keep timer wake-ups useful and prevent an invalid portal value from turning
// into an unexpectedly long (or zero-length) sleep.
constexpr uint32_t MIN_REFRESH_INTERVAL_MIN = 1;
constexpr uint32_t MAX_REFRESH_INTERVAL_MIN = 7 * 24 * 60;

// Fixed image endpoint. The provisioning page asks only for the server host;
// the firmware fills in the scheme, port, and path.
constexpr uint16_t SERVER_PORT = 35000;
constexpr const char *SERVER_PATH = "/epaper-display/image";

// Builds http://<host>:35000/epaper-display/image, or an empty string when no
// server has been configured.
String buildServerUrl(const DeviceConfig &cfg);

// Loads config from flash. Fills in sane defaults if nothing was saved yet.
void configLoad(DeviceConfig &cfg);

// Persists config to flash.
void configSave(const DeviceConfig &cfg);
