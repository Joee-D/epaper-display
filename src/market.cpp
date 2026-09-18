#include "market.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include <ArduinoJson.h>

// Mirror of TIME_SEGMENTS in main.py, including the chart tick labels. Times
// are ET minutes after midnight; the Evening window wraps past midnight
// (21:30 -> 00:00), which marketActiveSegment() handles.
const MarketSegment MARKET_SEGMENTS[] = {
    {"Premarket", "NQ=F", "NASDAQ FUTURES", 0, 570, 286, 3, {0, 150, 285},
     {"12:00", "17:00", "21:30"}},
    {"Regular", "^ndx", "NASDAQ 100", 570, 1060, 196, 3, {0, 105, 195},
     {"21:30", "01:00", "04:00"}},
    {"Evening", "930955.SS", "CSI Dividend Low Volatility 100", 1290, 0, 120, 3,
     {0, 60, 119}, {"09:30", "13:00", "15:00"}},
};

const int MARKET_SEGMENT_COUNT =
    sizeof(MARKET_SEGMENTS) / sizeof(MARKET_SEGMENTS[0]);

namespace {

constexpr const char *CHART_URL =
    "https://query1.finance.yahoo.com/v8/finance/chart/";
constexpr const char *ET_TZ = "EST5EDT,M3.2.0,M11.1.0";

// Symbols such as "^ndx" and "NQ=F" carry characters that must be escaped to
// stay valid inside a path segment.
String encodeSymbol(const char *symbol) {
  String encoded;
  for (const char *p = symbol; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      char escape[4];
      snprintf(escape, sizeof(escape), "%%%02X", c);
      encoded += escape;
    }
  }
  return encoded;
}

bool httpGet(const String &url, String &body) {
  WiFiClientSecure client;
  // Public quote data: skip chain validation so certificate rotation at
  // Yahoo's CDN cannot require a reflash.
  client.setInsecure();
  client.setTimeout(20);

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(20000);
  http.setUserAgent("Mozilla/5.0");
  http.addHeader("Accept-Encoding", "identity");
  if (!http.begin(client, url)) {
    Serial.println("Quote fetch failed: bad URL");
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Quote fetch failed: HTTP %d\n", code);
    http.end();
    return false;
  }

  body = http.getString();
  http.end();
  return true;
}

int minutesOfDay(const struct tm &t) { return t.tm_hour * 60 + t.tm_min; }

}  // namespace

bool marketSyncTime() {
  configTzTime(ET_TZ, "pool.ntp.org", "time.nist.gov");
  // getLocalTime() returns immediately when the clock is already sane, which is
  // the normal case once the device has synced once.
  struct tm now;
  if (getLocalTime(&now, 0)) return true;
  Serial.println("Waiting for NTP time sync...");
  return getLocalTime(&now, 20000);
}

bool marketNow(struct tm &nowEt) {
  const time_t now = time(nullptr);
  if (now < 1600000000) return false;  // clock never set
  localtime_r(&now, &nowEt);
  return true;
}

const MarketSegment *marketActiveSegment(const struct tm &nowEt) {
  const int minute = minutesOfDay(nowEt);
  for (int i = 0; i < MARKET_SEGMENT_COUNT; ++i) {
    const MarketSegment &segment = MARKET_SEGMENTS[i];
    if (segment.startMinute > segment.endMinute) {
      if (minute >= segment.startMinute || minute < segment.endMinute) {
        return &segment;
      }
    } else if (minute >= segment.startMinute && minute < segment.endMinute) {
      return &segment;
    }
  }
  return nullptr;
}

bool marketFetchSeries(const MarketSegment &segment, MarketSeries &out) {
  out.valid = false;
  out.count = 0;

  String url = CHART_URL;
  url += encodeSymbol(segment.symbol);
  url += "?range=1d&interval=2m";

  String body;
  if (!httpGet(url, body)) return false;

  // Only the fields the chart needs survive the transfer; a filtered parse
  // keeps the working set at a few KB instead of the whole ~20 KB response.
  JsonDocument filter;
  filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
  filter["chart"]["result"][0]["meta"]["previousClose"] = true;
  filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;

  JsonDocument document;
  const DeserializationError error =
      deserializeJson(document, body, DeserializationOption::Filter(filter));
  body = String();  // hand the ~20 KB payload back before rendering
  if (error) {
    Serial.printf("Quote JSON failed: %s\n", error.c_str());
    return false;
  }

  JsonObject result = document["chart"]["result"][0];
  if (result.isNull()) {
    Serial.println("Quote JSON failed: no result for symbol");
    return false;
  }

  float prevClose = result["meta"]["previousClose"] | 0.0f;
  if (prevClose <= 0.0f) {
    prevClose = result["meta"]["chartPreviousClose"] | 0.0f;
  }
  float price = result["meta"]["regularMarketPrice"] | 0.0f;

  JsonArray closes = result["indicators"]["quote"][0]["close"];
  int count = 0;
  for (JsonVariant close : closes) {
    if (count >= MARKET_MAX_POINTS) break;
    if (close.isNull()) continue;  // mirrors pandas dropna()
    out.closes[count++] = close.as<float>();
  }

  if (count < 2) {
    Serial.println("Quote JSON failed: not enough points");
    return false;
  }
  if (count > segment.pointLimit) count = segment.pointLimit;  // prices[:max_len]

  if (price <= 0.0f) price = out.closes[count - 1];
  if (prevClose <= 0.0f) prevClose = out.closes[0];

  out.price = price;
  out.prevClose = prevClose;
  out.count = count;
  out.valid = true;
  return true;
}
