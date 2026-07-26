#pragma once
#include <Arduino.h>

// Persisted device configuration (stored in NVS via Preferences).
struct DeviceConfig {
  String   serverUrl;     // e.g. https://myserver.example.com/epaper-display/image
  uint32_t intervalMin;   // how often to wake up + fetch, in minutes
  bool     rotate180;     // flip image 180 degrees if the panel is mounted upside down
};

// Loads config from flash. Fills in sane defaults if nothing was saved yet.
void configLoad(DeviceConfig &cfg);

// Persists config to flash.
void configSave(const DeviceConfig &cfg);

// Erases saved config AND saved WiFi credentials, forcing the setup portal
// to run again on next boot.
void configFactoryReset();
