#include "display.h"
#include <SPI.h>
#include <HTTPClient.h>
#include <WiFiClient.h>

#include <GxEPD2_BW.h>
#include <gdey/GxEPD2_420_GDEY042T81.h>

static GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> epd(
    GxEPD2_420_GDEY042T81(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

RTC_DATA_ATTR static int refreshCounter = 0; 

// 定义每快刷多少次后，执行一次全屏刷新清除残影（可根据实际残影严重程度修改）
const int FULL_REFRESH_INTERVAL = 5; 

void displayInit() {
  SPI.begin(PIN_EPD_SCK, /*MISO=*/-1, PIN_EPD_MOSI, PIN_EPD_CS);
  
  bool isFullRefresh = (refreshCounter == 0);
  
  epd.init(115200, isFullRefresh, /*resetDuration=*/20, /*pulldown_rst_mode=*/false);
  epd.setRotation(0); 
}

void displayShowMessage(const String &line1, const String &line2) {
  // 根据当前初始化状态选择窗口模式
  if (refreshCounter == 0) {
    epd.setFullWindow();
  } else {
    epd.setPartialWindow(0, 0, epd.width(), epd.height());
  }

  epd.firstPage();
  do {
    epd.fillScreen(GxEPD_WHITE);
    epd.setTextColor(GxEPD_BLACK);
    epd.setTextSize(2);

    int16_t x1, y1;
    uint16_t w, h;
    epd.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
    epd.setCursor((epd.width() - w) / 2, epd.height() / 2 - 20);
    epd.print(line1);

    epd.setTextSize(1);
    epd.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
    epd.setCursor((epd.width() - w) / 2, epd.height() / 2 + 10);
    epd.print(line2);
  } while (epd.nextPage());
}

bool displayFetchAndShow(const DeviceConfig &cfg, const String &deviceId) {
  if (cfg.serverUrl.length() == 0) {
    displayShowMessage("No server configured", "Set it up at 192.168.4.1");
    return false;
  }

  String url = cfg.serverUrl;
  url += (url.indexOf('?') >= 0) ? '&' : '?';
  url += "w=" + String(EPD_WIDTH) + "&h=" + String(EPD_HEIGHT) + "&id=" + deviceId;

  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(url)) {
    displayShowMessage("Bad server URL", url);
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    displayShowMessage("Fetch failed", "HTTP " + String(code));
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len != EPD_IMAGE_BYTES) {
    displayShowMessage("Bad image size", String(len) + " != " + String(EPD_IMAGE_BYTES));
    http.end();
    return false;
  }

  uint8_t *buf = (uint8_t *)malloc(EPD_IMAGE_BYTES);
  if (!buf) {
    displayShowMessage("Out of memory", "");
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t received = 0;
  unsigned long start = millis();
  while (received < (size_t)EPD_IMAGE_BYTES && millis() - start < 20000) {
    if (stream->available()) {
      int n = stream->read(buf + received, EPD_IMAGE_BYTES - received);
      if (n > 0) received += n;
    } else {
      delay(5);
    }
  }
  http.end();

  if (received != (size_t)EPD_IMAGE_BYTES) {
    free(buf);
    displayShowMessage("Incomplete download", String(received) + "/" + String(EPD_IMAGE_BYTES));
    return false;
  }

  if (refreshCounter == 0) {
    epd.setFullWindow(); // 全屏黑白闪烁刷洗
  } else {
    epd.setPartialWindow(0, 0, EPD_WIDTH, EPD_HEIGHT); // 无闪烁快速局刷
  }

  epd.firstPage();
  do {
    epd.fillScreen(GxEPD_WHITE);
    epd.drawBitmap(0, 0, buf, EPD_WIDTH, EPD_HEIGHT, GxEPD_BLACK);
  } while (epd.nextPage());

  free(buf);

  refreshCounter++;
  if (refreshCounter > FULL_REFRESH_INTERVAL) {
    refreshCounter = 0; // 达到设定次数，下次唤醒时触发全刷
  }

  return true;
}

void displayHibernate() {
  epd.hibernate();
}
