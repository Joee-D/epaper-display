#pragma once
#include <Arduino.h>

// Widest chart segment plots at most ticks[-1] + 1 = 286 intraday points.
constexpr int MARKET_MAX_POINTS = 288;

// One of the daily charts, mirroring TIME_SEGMENTS in main.py.
struct MarketSegment {
  const char *name;         // log label
  const char *symbol;       // Yahoo Finance symbol
  const char *displayName;  // bold title drawn on the chart
  uint16_t    startMinute;  // America/New_York window, minutes after midnight
  uint16_t    endMinute;
  int         pointLimit;   // xticks[-1] + 1, as used by the server
  int         tickCount;
  int         ticks[3];
  const char *tickLabels[3];
};

// One session of intraday closes plus the baseline the chart compares against.
struct MarketSeries {
  float price;      // latest price (meta.regularMarketPrice)
  float prevClose;  // official previous close (meta.previousClose)
  int   count;
  float closes[MARKET_MAX_POINTS];
};

extern const MarketSegment MARKET_SEGMENTS[];

// Points the clock at NTP and applies the ET timezone. False if the clock could
// not be set, in which case market windows cannot be evaluated.
bool marketSyncTime();

// Fills `nowEt` with the current America/New_York wall time.
bool marketNow(struct tm &nowEt);

// The segment whose ET window covers `nowEt`, or nullptr while every market is
// closed. Mirrors is_trading_now() in main.py, wrap past midnight included.
const MarketSegment *marketActiveSegment(const struct tm &nowEt);

// Downloads one session of intraday prices. False on any failure (network,
// JSON, or too few points), leaving `out` unmodified in shape.
bool marketFetchSeries(const MarketSegment &segment, MarketSeries &out);
