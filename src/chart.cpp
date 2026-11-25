#include "chart.h"

#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>
#include <string.h>

#include "display.h"

namespace {

constexpr uint16_t INK = 1;
constexpr uint16_t PAPER = 0;

// Geometry follows the original 400x300 matplotlib layout.
constexpr float CHART_X0 = 20.28f, CHART_X1 = 379.72f;
constexpr float CHART_TOP = 94.48f, CHART_BOTTOM = 282.64f;
constexpr float TEXT_LEFT = 27.47f;
constexpr float TITLE_TOP = 25.0f;
constexpr float PRICE_TOP = 59.0f;
constexpr float BADGE_CENTER_Y = 69.5f;
constexpr float BADGE_RIGHT = 372.5f;
constexpr int BADGE_PAD_X = 6;
constexpr int BADGE_PAD_Y = 3;
constexpr int TICK_LABEL_TOP = 288;

constexpr float FILL_LIGHT = 250.0f;
constexpr float FILL_DARK = 176.0f;
constexpr float INK_THRESHOLD = 128.0f;
constexpr int CHART_LINE_WIDTH = 3;

struct FillScratch {
  int16_t spanTop[EPD_WIDTH];
  int16_t spanBottom[EPD_WIDTH];
  float carried[EPD_WIDTH + 2];
  float pending[EPD_WIDTH + 2];
} scratch;

struct Plot {
  float xMax;
  float yLo, yHi;

  float x(float index) const {
    return CHART_X0 + index / xMax * (CHART_X1 - CHART_X0);
  }
  float y(float price) const {
    return CHART_TOP +
           (yHi - price) / (yHi - yLo) * (CHART_BOTTOM - CHART_TOP);
  }
  float priceAtRow(float row) const {
    return yHi -
           (row - CHART_TOP) / (CHART_BOTTOM - CHART_TOP) * (yHi - yLo);
  }
};

Plot makePlot(const MarketSegment &segment, const MarketSeries &series) {
  float low = series.prevClose, high = series.prevClose;
  for (int i = 0; i < series.count; ++i) {
    low = fminf(low, series.closes[i]);
    high = fmaxf(high, series.closes[i]);
  }
  float margin = (high - low) * 0.1f;
  if (margin <= 0.0f) margin = 0.001f;
  return {(float)segment.ticks[segment.tickCount - 1], low - margin,
          high + margin};
}

void formatPrice(float value, char *out, size_t size) {
  const long long cents = llroundf(fabsf(value) * 100.0f);
  snprintf(out, size, "%s%lld.%02d", value < 0.0f ? "-" : "", cents / 100,
           (int)(cents % 100));
}

struct TextBox {
  int16_t x, y;
  uint16_t width, height;
};

TextBox textBox(GFXcanvas1 &canvas, const char *text) {
  TextBox box;
  canvas.getTextBounds(text, 0, 0, &box.x, &box.y, &box.width, &box.height);
  return box;
}

void drawText(GFXcanvas1 &canvas, float left, float top, const char *text) {
  const TextBox box = textBox(canvas, text);
  canvas.setCursor((int16_t)lroundf(left) - box.x,
                   (int16_t)lroundf(top) - box.y);
  canvas.print(text);
}

void drawTextCentered(GFXcanvas1 &canvas, float centerX, float top,
                      const char *text) {
  drawText(canvas, centerX - textBox(canvas, text).width / 2.0f, top, text);
}

void drawHeader(GFXcanvas1 &canvas, const MarketSegment &segment,
                const MarketSeries &series) {
  char price[32];
  formatPrice(series.price, price, sizeof(price));
  char badge[24];
  snprintf(badge, sizeof(badge), "%+.2f%%",
           (series.price - series.prevClose) / series.prevClose * 100.0f);

  canvas.setFont(&FreeSansBold9pt7b);
  drawText(canvas, TEXT_LEFT, TITLE_TOP, segment.displayName);

  canvas.setFont(&FreeSansBold18pt7b);
  drawText(canvas, TEXT_LEFT, PRICE_TOP, price);

  canvas.setFont(&FreeSansBold12pt7b);
  const TextBox box = textBox(canvas, badge);
  const int width = box.width + 2 * BADGE_PAD_X;
  const int height = box.height + 2 * BADGE_PAD_Y;
  const int left = (int)lroundf(BADGE_RIGHT) - width;
  const int top = (int)lroundf(BADGE_CENTER_Y - height / 2.0f);
  canvas.fillRoundRect(left, top, width, height, 4, INK);
  canvas.setTextColor(PAPER, INK);
  canvas.setCursor(left + BADGE_PAD_X - box.x, top + BADGE_PAD_Y - box.y);
  canvas.print(badge);
  canvas.setTextColor(INK, PAPER);
}

}  // namespace

bool chartDisplay(const MarketSegment &segment, const MarketSeries &series) {
  const int count = series.count;
  if (count < 2) return false;

  GFXcanvas1 canvas(EPD_WIDTH, EPD_HEIGHT);
  if (canvas.getBuffer() == nullptr) {
    Serial.println("Chart failed: no framebuffer");
    return false;
  }
  canvas.fillScreen(PAPER);
  canvas.setTextSize(1);
  canvas.setTextColor(INK, PAPER);

  drawHeader(canvas, segment, series);

  const Plot plot = makePlot(segment, series);
  const float baselineY = plot.y(series.prevClose);
  const int chartTop = (int)CHART_TOP;
  const int chartBottom = (int)CHART_BOTTOM;

  for (int x = (int)CHART_X0; x <= (int)CHART_X1; x += 3) {
    canvas.drawPixel(x, (int)lroundf(baselineY), INK);
  }

  uint8_t grey[EPD_HEIGHT];
  for (int row = chartTop; row <= chartBottom; ++row) {
    const float price = plot.priceAtRow(row);
    const float span = price >= series.prevClose ? plot.yHi - series.prevClose
                                                : series.prevClose - plot.yLo;
    float t = span > 0.0f ? fabsf(price - series.prevClose) / span : 0.0f;
    t = t > 1.0f ? 1.0f : t;
    grey[row] = (uint8_t)lroundf(FILL_LIGHT + (FILL_DARK - FILL_LIGHT) * t);
  }

  memset(&scratch, 0, sizeof(scratch));
  for (int x = 0; x < EPD_WIDTH; ++x) {
    scratch.spanTop[x] = 1;
    scratch.spanBottom[x] = 0;

    const float position =
        ((float)x - CHART_X0) / (CHART_X1 - CHART_X0) * plot.xMax;
    if (position < 0.0f || position > (float)(count - 1)) continue;

    const int index = (int)position;
    const int next = index + 1 < count ? index + 1 : index;
    const float fraction = position - (float)index;
    const float value = series.closes[index] +
                        (series.closes[next] - series.closes[index]) * fraction;
    const float curveY = plot.y(value);

    int from = (int)lroundf(fminf(curveY, baselineY));
    int to = (int)lroundf(fmaxf(curveY, baselineY));
    if (from < chartTop) from = chartTop;
    if (to > chartBottom) to = chartBottom;
    if (from > to) continue;
    scratch.spanTop[x] = (int16_t)from;
    scratch.spanBottom[x] = (int16_t)to;
  }

  {
    float *carried = scratch.carried;
    float *pending = scratch.pending;
    for (int y = chartTop; y <= chartBottom; ++y) {
      for (int x = 0; x < EPD_WIDTH; ++x) {
        const bool inFill =
            y >= scratch.spanTop[x] && y <= scratch.spanBottom[x];
        const float target = inFill ? (float)grey[y] : 255.0f;
        float value = target + carried[x + 1];
        value = value < 0.0f ? 0.0f : (value > 255.0f ? 255.0f : value);
        const bool ink = value < INK_THRESHOLD;
        const float error = value - (ink ? 0.0f : 255.0f);
        if (ink) canvas.drawPixel(x, y, INK);
        carried[x + 2] += error * 7.0f / 16.0f;
        pending[x] += error * 3.0f / 16.0f;
        pending[x + 1] += error * 5.0f / 16.0f;
        pending[x + 2] += error / 16.0f;
      }
      float *swap = carried;
      carried = pending;
      pending = swap;
      memset(pending, 0, (EPD_WIDTH + 2) * sizeof(float));
    }
  }

  for (int i = 1; i < count; ++i) {
    const int x0 = (int)lroundf(plot.x(i - 1));
    const int x1 = (int)lroundf(plot.x(i));
    const int y0 = (int)lroundf(plot.y(series.closes[i - 1]));
    const int y1 = (int)lroundf(plot.y(series.closes[i]));
    for (int offset = -(CHART_LINE_WIDTH / 2);
         offset <= CHART_LINE_WIDTH / 2; ++offset) {
      canvas.drawLine(x0, y0 + offset, x1, y1 + offset, INK);
    }
  }

  canvas.setFont(nullptr);
  for (int i = 0; i < segment.tickCount; ++i) {
    drawTextCentered(canvas, plot.x(segment.ticks[i]), TICK_LABEL_TOP,
                     series.tickLabels[i]);
  }

  return displayShowBitmap(canvas.getBuffer());
}
