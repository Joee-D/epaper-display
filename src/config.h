#pragma once
#include <Arduino.h>

// Persisted device configuration (stored in NVS via Preferences).
struct DeviceConfig {
  String   serverIp;      // image server IP only, e.g. "192.168.1.50"
  uint32_t intervalMin;   // how often to wake up + fetch, in minutes
  bool     rotate180;     // flip image 180 degrees if the panel is mounted upside down
};

// Keep timer wake-ups useful and prevent an invalid portal value from turning
// into an unexpectedly long (or zero-length) sleep.
constexpr uint32_t MIN_REFRESH_INTERVAL_MIN = 1;
constexpr uint32_t MAX_REFRESH_INTERVAL_MIN = 7 * 24 * 60;

// Fixed image-endpoint details the firmware fills in automatically during
// setup; only the server IP is entered on the portal page.
constexpr uint16_t SERVER_PORT = 35000;
constexpr const char *SERVER_PATH = "/epaper-display/image";

// Builds the full image URL (http://<ip>:35000/epaper-display/image) from the
// stored server IP. Returns an empty string when no IP is configured.
String buildServerUrl(const DeviceConfig &cfg);

// Loads config from flash. Fills in sane defaults if nothing was saved yet.
void configLoad(DeviceConfig &cfg);

// Persists config to flash.
void configSave(const DeviceConfig &cfg);

// Erases saved config AND saved WiFi credentials, forcing the setup portal
// to run again on next boot.
void configFactoryReset();
