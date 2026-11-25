#pragma once
#include <Arduino.h>
#include <time.h>

constexpr int MARKET_MAX_POINTS = 288;

struct MarketWindow {
  uint16_t startMinute;
  uint16_t endMinute;
};

struct MarketSegment {
  const char *name;
  const char *symbol;
  const char *displayName;
  const char *timezone;
  uint8_t windowCount;
  MarketWindow windows[2];
  int pointLimit;
  int tickCount;
  int ticks[3];
  uint16_t axisMinutes[3];
  bool includePrePost;
  bool futuresCalendar;
};

struct MarketSeries {
  float price;
  float prevClose;
  int count;
  float closes[MARKET_MAX_POINTS];
  char tickLabels[3][6];
};

extern const MarketSegment MARKET_SEGMENTS[];

bool marketSyncTime();
const MarketSegment *marketActiveSegment(time_t now, struct tm &marketNow);
bool marketFetchSeries(const MarketSegment &segment, time_t now,
                       MarketSeries &out);
