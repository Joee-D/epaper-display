#include "config.h"
#include "firmware_config.h"

static const uint32_t DEFAULT_INTERVAL_MIN = 15;

static uint32_t normalizedInterval(uint32_t intervalMin) {
  if (intervalMin < MIN_REFRESH_INTERVAL_MIN ||
      intervalMin > MAX_REFRESH_INTERVAL_MIN) {
    return DEFAULT_INTERVAL_MIN;
  }
  return intervalMin;
}

void configLoad(DeviceConfig &cfg) {
  cfg.wifiSsid     = EPAPER_WIFI_SSID;
  cfg.wifiPassword = EPAPER_WIFI_PASSWORD;
  cfg.intervalMin = normalizedInterval(EPAPER_REFRESH_INTERVAL_MIN);
}
