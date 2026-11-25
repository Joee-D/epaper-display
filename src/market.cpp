#include "market.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>
#include <string.h>

#include <ArduinoJson.h>

namespace {

constexpr const char *CHART_PATH = "/v8/finance/chart/";
constexpr const char *CHART_HOSTS[] = {
    "query1.finance.yahoo.com",
    "query2.finance.yahoo.com",
};
constexpr const char *ET_TZ = "EST5EDT,M3.2.0,M11.1.0";
constexpr const char *CN_TZ = "CST-8";
constexpr const char *DISPLAY_TZ = CN_TZ;
constexpr const char *QUOTE_USER_AGENT = "Mozilla/5.0";
constexpr time_t MIN_VALID_EPOCH = 1600000000;
constexpr uint32_t NTP_RESYNC_SECONDS = 6 * 60 * 60;

enum class HttpResult {
  Ok,
  Transient,
  Permanent,
};

struct ClosedDate {
  uint16_t year;
  uint8_t month;
  uint8_t day;
};

const ClosedDate US_CLOSED_DATES[] = {
    {2026, 1, 1},   {2026, 1, 19},  {2026, 2, 16},  {2026, 4, 3},
    {2026, 5, 25},  {2026, 6, 19},  {2026, 7, 3},   {2026, 9, 7},
    {2026, 11, 26}, {2026, 12, 25}, {2027, 1, 1},   {2027, 1, 18},
    {2027, 2, 15},  {2027, 3, 26},  {2027, 5, 31},  {2027, 6, 18},
    {2027, 7, 5},   {2027, 9, 6},   {2027, 11, 25}, {2027, 12, 24},
    {2028, 1, 17},  {2028, 2, 21},  {2028, 4, 14},  {2028, 5, 29},
    {2028, 6, 19},  {2028, 7, 4},   {2028, 9, 4},   {2028, 11, 23},
    {2028, 12, 25}, {2029, 1, 1},   {2029, 1, 15},  {2029, 2, 19},
    {2029, 3, 30},  {2029, 5, 28},  {2029, 6, 19},  {2029, 7, 4},
    {2029, 9, 3},   {2029, 11, 22}, {2029, 12, 25}, {2030, 1, 1},
    {2030, 1, 21},  {2030, 2, 18},  {2030, 4, 19},  {2030, 5, 27},
    {2030, 6, 19},  {2030, 7, 4},   {2030, 9, 2},   {2030, 11, 28},
    {2030, 12, 25},
};

const ClosedDate CME_CLOSED_DATES[] = {
    {2026, 1, 1},   {2026, 4, 3},   {2026, 12, 25}, {2027, 1, 1},
    {2027, 3, 26},  {2027, 12, 24}, {2028, 4, 14},  {2028, 12, 25},
    {2029, 1, 1},   {2029, 3, 30},  {2029, 12, 25}, {2030, 1, 1},
    {2030, 4, 19},  {2030, 12, 25},
};

const ClosedDate CN_CLOSED_DATES[] = {
    {2026, 1, 1},   {2026, 1, 2},   {2026, 2, 16},  {2026, 2, 17},
    {2026, 2, 18},  {2026, 2, 19},  {2026, 2, 20},  {2026, 4, 6},
    {2026, 5, 1},   {2026, 6, 19},  {2026, 9, 25},  {2026, 10, 1},
    {2026, 10, 2},  {2026, 10, 5},  {2026, 10, 6},  {2026, 10, 7},
    {2027, 1, 1},   {2027, 2, 5},   {2027, 2, 8},   {2027, 2, 9},
    {2027, 2, 10},  {2027, 2, 11},  {2027, 4, 5},   {2027, 5, 3},
    {2027, 6, 9},   {2027, 9, 15},  {2027, 10, 1},  {2027, 10, 4},
    {2027, 10, 5},  {2027, 10, 6},  {2027, 10, 7},  {2028, 1, 3},
    {2028, 1, 25},  {2028, 1, 26},  {2028, 1, 27},  {2028, 1, 28},
    {2028, 1, 31},  {2028, 4, 3},   {2028, 4, 4},   {2028, 5, 1},
    {2028, 5, 29},  {2028, 10, 2},  {2028, 10, 3},  {2028, 10, 4},
    {2028, 10, 5},  {2028, 10, 6},  {2029, 1, 1},   {2029, 2, 12},
    {2029, 2, 13},  {2029, 2, 14},  {2029, 2, 15},  {2029, 2, 16},
    {2029, 4, 4},   {2029, 4, 30},  {2029, 5, 1},   {2029, 6, 18},
    {2029, 9, 24},  {2029, 10, 1},  {2029, 10, 2},  {2029, 10, 3},
    {2029, 10, 4},  {2029, 10, 5},  {2029, 12, 31}, {2030, 1, 1},
    {2030, 2, 4},   {2030, 2, 5},   {2030, 2, 6},   {2030, 2, 7},
    {2030, 2, 8},   {2030, 4, 5},   {2030, 5, 1},   {2030, 6, 5},
    {2030, 9, 12},  {2030, 9, 13},  {2030, 10, 1},  {2030, 10, 2},
    {2030, 10, 3},  {2030, 10, 4},  {2030, 10, 7},
};

time_t lastNtpSync = 0;

void setTimezone(const char *timezone) {
  setenv("TZ", timezone, 1);
  tzset();
}

bool marketTime(const MarketSegment &segment, time_t value, struct tm &out) {
  setTimezone(segment.timezone);
  return localtime_r(&value, &out) != nullptr;
}

int minutesOfDay(const struct tm &value) {
  return value.tm_hour * 60 + value.tm_min;
}

bool inWindow(const MarketSegment &segment, int minute) {
  for (int i = 0; i < segment.windowCount; ++i) {
    const MarketWindow &window = segment.windows[i];
    if (minute >= window.startMinute && minute < window.endMinute) return true;
  }
  return false;
}

bool inClosedDates(const ClosedDate *dates, size_t count, const struct tm &value) {
  for (size_t i = 0; i < count; ++i) {
    if (dates[i].year == value.tm_year + 1900 &&
        dates[i].month == value.tm_mon + 1 && dates[i].day == value.tm_mday) {
      return true;
    }
  }
  return false;
}

bool isMarketHoliday(const MarketSegment &segment, const struct tm &value) {
  if (strcmp(segment.timezone, CN_TZ) == 0) {
    return inClosedDates(
        CN_CLOSED_DATES,
        sizeof(CN_CLOSED_DATES) / sizeof(CN_CLOSED_DATES[0]),
        value);
  }
  if (segment.futuresCalendar) {
    return inClosedDates(
        CME_CLOSED_DATES,
        sizeof(CME_CLOSED_DATES) / sizeof(CME_CLOSED_DATES[0]),
        value);
  }
  return inClosedDates(
      US_CLOSED_DATES,
      sizeof(US_CLOSED_DATES) / sizeof(US_CLOSED_DATES[0]),
      value);
}

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

bool isTransientStatus(int code) {
  return code < 0 || code == 403 || code == 408 || code == 425 ||
         code == 429 || code >= 500;
}

HttpResult httpGetJson(const String &url, const JsonDocument &filter,
                       JsonDocument &document) {
  Serial.println("Yahoo URL: " + url);

  WiFiClientSecure client;
  // Public market data; avoid pinning Yahoo's rotating CDN certificates.
  client.setInsecure();
  client.setTimeout(20);

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(20000);
  http.setUserAgent(QUOTE_USER_AGENT);
  // Avoid chunked transfer encoding so ArduinoJson can parse the network
  // stream directly without buffering the full response in a String.
  http.useHTTP10(true);
  http.addHeader("Accept", "*/*");
  http.addHeader("Accept-Language", "en-US,en;q=0.9");
  http.addHeader("Referer", "https://finance.yahoo.com/");
  if (!http.begin(client, url)) {
    Serial.println("Yahoo request failed: bad URL");
    return HttpResult::Transient;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Yahoo request failed: HTTP %d %s\n", code,
                  http.errorToString(code).c_str());
    const String server = http.header("Server");
    if (server.length() > 0) Serial.println("Yahoo Server: " + server);
    String reason = http.getString();
    if (reason.length() > 200) reason = reason.substring(0, 200);
    reason.replace("\n", " ");
    reason.replace("\r", "");
    if (reason.length() > 0) Serial.println("Yahoo body: " + reason);
    http.end();
    return isTransientStatus(code) ? HttpResult::Transient
                                   : HttpResult::Permanent;
  }

  Serial.printf("Yahoo HTTP size: %d\n", http.getSize());
  const DeserializationError error = deserializeJson(
      document, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (error) {
    Serial.printf("Yahoo JSON failed: %s\n", error.c_str());
    return HttpResult::Permanent;
  }
  return HttpResult::Ok;
}

void buildTickLabels(const MarketSegment &segment, time_t now,
                     MarketSeries &series) {
  for (int i = 0; i < segment.tickCount; ++i) {
    snprintf(series.tickLabels[i], sizeof(series.tickLabels[i]), "--:--");
  }

  setTimezone(segment.timezone);
  struct tm marketNow;
  if (localtime_r(&now, &marketNow) == nullptr) return;

  for (int i = 0; i < segment.tickCount; ++i) {
    struct tm target = marketNow;
    target.tm_hour = segment.axisMinutes[i] / 60;
    target.tm_min = segment.axisMinutes[i] % 60;
    target.tm_sec = 0;
    target.tm_isdst = -1;

    setTimezone(segment.timezone);
    const time_t targetUtc = mktime(&target);
    if (targetUtc == static_cast<time_t>(-1)) continue;
    setTimezone(DISPLAY_TZ);
    struct tm displayTime;
    if (localtime_r(&targetUtc, &displayTime) == nullptr) continue;
    snprintf(series.tickLabels[i], sizeof(series.tickLabels[i]), "%02d:%02d",
             displayTime.tm_hour, displayTime.tm_min);
  }
}

}  // namespace

// Order is display priority: local A-share, U.S. regular, U.S. premarket.
const MarketSegment MARKET_SEGMENTS[] = {
    {"Evening", "930955.SS", "CSI Dividend Low Volatility 100", CN_TZ, 2,
     {{570, 690}, {780, 900}}, 120, 3, {0, 60, 119}, {570, 780, 900}, false,
     false},
    {"Regular", "^ndx", "NASDAQ 100", ET_TZ, 1, {{570, 1060}}, 196, 3,
     {0, 105, 195}, {570, 780, 960}, false, false},
    {"Premarket", "NQ=F", "NASDAQ FUTURES", ET_TZ, 1, {{0, 570}}, 286, 3,
     {0, 150, 285}, {0, 300, 570}, true, true},
};

bool marketSyncTime() {
  const time_t now = time(nullptr);
  if (now >= MIN_VALID_EPOCH && lastNtpSync > 0 &&
      now - lastNtpSync < NTP_RESYNC_SECONDS) {
    return true;
  }

  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
  struct tm ignored;
  if (!getLocalTime(&ignored, 0)) {
    Serial.println("Waiting for NTP time sync");
    if (!getLocalTime(&ignored, 20000)) return false;
  }
  lastNtpSync = time(nullptr);
  return lastNtpSync >= MIN_VALID_EPOCH;
}

const MarketSegment *marketActiveSegment(time_t now, struct tm &marketNow) {
  for (const MarketSegment &segment : MARKET_SEGMENTS) {
    if (!marketTime(segment, now, marketNow)) continue;
    if (marketNow.tm_wday == 0 || marketNow.tm_wday == 6) continue;
    if (isMarketHoliday(segment, marketNow)) continue;
    if (inWindow(segment, minutesOfDay(marketNow))) return &segment;
  }
  return nullptr;
}

bool marketFetchSeries(const MarketSegment &segment, time_t now,
                       MarketSeries &out) {
  out.count = 0;

  JsonDocument filter;
  filter["chart"]["result"][0]["meta"]["regularMarketPrice"] = true;
  filter["chart"]["result"][0]["meta"]["previousClose"] = true;
  filter["chart"]["result"][0]["meta"]["chartPreviousClose"] = true;
  filter["chart"]["result"][0]["timestamp"] = true;
  filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;

  JsonDocument document;
  bool downloaded = false;
  for (const char *host : CHART_HOSTS) {
    String url = "https://";
    url += host;
    url += CHART_PATH;
    url += encodeSymbol(segment.symbol);
    url += "?range=1d&interval=2m&includePrePost=";
    url += segment.includePrePost ? "true" : "false";
    const HttpResult result = httpGetJson(url, filter, document);
    if (result == HttpResult::Ok) {
      downloaded = true;
      break;
    }
    if (result == HttpResult::Permanent) return false;
  }
  if (!downloaded) return false;

  JsonObject result = document["chart"]["result"][0];
  if (result.isNull()) {
    Serial.println("Yahoo JSON failed: no result");
    return false;
  }

  float prevClose = result["meta"]["previousClose"] | 0.0f;
  if (prevClose <= 0.0f) prevClose = result["meta"]["chartPreviousClose"] | 0.0f;
  float price = result["meta"]["regularMarketPrice"] | 0.0f;

  JsonArray timestamps = result["timestamp"];
  JsonArray closes = result["indicators"]["quote"][0]["close"];
  const size_t pointCount = min(timestamps.size(), closes.size());

  setTimezone(segment.timezone);
  struct tm today;
  localtime_r(&now, &today);
  const int todayYear = today.tm_year;
  const int todayDay = today.tm_yday;

  int count = 0;
  for (size_t i = 0; i < pointCount && count < segment.pointLimit; ++i) {
    if (closes[i].isNull()) continue;
    const time_t timestamp = timestamps[i].as<time_t>();
    if (timestamp <= 0) continue;
    struct tm barTime;
    if (localtime_r(&timestamp, &barTime) == nullptr) continue;
    if (barTime.tm_year != todayYear || barTime.tm_yday != todayDay) continue;
    if (!inWindow(segment, minutesOfDay(barTime))) continue;
    out.closes[count++] = closes[i].as<float>();
  }

  if (count < 2) {
    Serial.println("Yahoo data failed: not enough points");
    return false;
  }
  if (price <= 0.0f) price = out.closes[count - 1];
  if (prevClose <= 0.0f) prevClose = out.closes[0];

  out.price = price;
  out.prevClose = prevClose;
  out.count = count;
  buildTickLabels(segment, now, out);
  return true;
}
