#pragma once
#include <Arduino.h>

// Device configuration compiled from firmware_config.h.
struct DeviceConfig {
  String   wifiSsid;
  String   wifiPassword;
  uint32_t intervalMin;   // how often to wake up + fetch, in minutes
};

// Keep timer wake-ups useful and prevent an invalid config value from turning
// into an unexpectedly long (or zero-length) sleep.
constexpr uint32_t MIN_REFRESH_INTERVAL_MIN = 1;
constexpr uint32_t MAX_REFRESH_INTERVAL_MIN = 7 * 24 * 60;

// Copies the compile-time settings into the runtime configuration.
void configLoad(DeviceConfig &cfg);
