#include "chart.h"

#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>
#include <string.h>

#include "display.h"

namespace {

// GFXcanvas1 sets a bit for any non-zero colour and GxEPD2::drawBitmap() paints
// a set bit in the colour it is given, so 1 = ink keeps both sides in the same
// convention as the bitmap the image server used to send.
constexpr uint16_t INK = 1;
constexpr uint16_t PAPER = 0;

// Geometry measured from the server-side matplotlib figure (figsize 4x3 at
// dpi 100, gridspec height_ratios [1, 2.2], tight_layout(pad=0.2)).
constexpr float TEXT_X0 = 20.28f, TEXT_X1 = 379.72f;
constexpr float TEXT_TOP = 2.78f, TEXT_BOTTOM = 88.31f;
constexpr float CHART_X0 = 20.28f, CHART_X1 = 379.72f;
constexpr float CHART_TOP = 94.48f, CHART_BOTTOM = 282.64f;
constexpr float TEXT_LEFT = 27.47f;      // 0.02 of the text axes
// 9pt bold caps are 12px tall, within a pixel of the server's 11pt text.
constexpr float TITLE_TOP = 25.0f;       // baseline 37 in the matplotlib layout
constexpr float PRICE_TOP = 59.0f;       // fontsize 20 at 0.05
constexpr float BADGE_CENTER_Y = 69.5f;  // fontsize 11 at 0.15
constexpr float BADGE_RIGHT = 372.5f;    // 0.98 of the text axes, right aligned
constexpr int BADGE_PAD_X = 6;
constexpr int BADGE_PAD_Y = 3;
constexpr int TICK_LABEL_TOP = 288;      // just below the chart axes

// matplotlib fades #fafafa -> #b0b0b0 moving away from the baseline.
constexpr float FILL_LIGHT = 250.0f;
constexpr float FILL_DARK = 176.0f;

// Ordered 4x4 Bayer thresholds: a pixel turns black when its grey level is
// below the threshold, which reproduces the server's dithered gradient without
// keeping an 8-bit copy of the image around.
const uint8_t BAYER4[4][4] = {
    {0, 136, 34, 170},
    {204, 68, 238, 102},
    {51, 187, 17, 153},
    {255, 119, 221, 85},
};

GFXcanvas1 *gCanvas = nullptr;

float chartX(int index, int xMax) {
  return CHART_X0 + (float)index / (float)xMax * (CHART_X1 - CHART_X0);
}

float chartY(float value, float yLo, float yHi) {
  return CHART_TOP + (yHi - value) / (yHi - yLo) * (CHART_BOTTOM - CHART_TOP);
}

// "29,843.25", matching f"{value:,.2f}" on the server.
void formatPrice(float value, char *out, size_t size) {
  const long long cents = llroundf(fabsf(value) * 100.0f);
  char digits[24];
  snprintf(digits, sizeof(digits), "%lld", cents / 100);

  char grouped[32];
  const size_t length = strlen(digits);
  size_t used = 0;
  for (size_t i = 0; i < length; ++i) {
    if (i > 0 && (length - i) % 3 == 0) grouped[used++] = ',';
    grouped[used++] = digits[i];
  }
  grouped[used] = '\0';

  snprintf(out, size, "%s%s.%02d", value < 0.0f ? "-" : "", grouped,
           (int)(cents % 100));
}

// Places text so its ink starts exactly at (x, y), which removes any guesswork
// about where a given font measures its origin.
void drawText(GFXcanvas1 &canvas, float x, float y, const char *text) {
  int16_t boundsX, boundsY;
  uint16_t width, height;
  canvas.getTextBounds(text, 0, 0, &boundsX, &boundsY, &width, &height);
  canvas.setCursor((int16_t)lroundf(x) - boundsX, (int16_t)lroundf(y) - boundsY);
  canvas.print(text);
}

void drawTextCentered(GFXcanvas1 &canvas, float centerX, float y,
                      const char *text) {
  int16_t boundsX, boundsY;
  uint16_t width, height;
  canvas.getTextBounds(text, 0, 0, &boundsX, &boundsY, &width, &height);
  canvas.setCursor((int16_t)lroundf(centerX - width / 2.0f) - boundsX,
                   (int16_t)lroundf(y) - boundsY);
  canvas.print(text);
}

}  // namespace

const uint8_t *chartRender(const MarketSegment &segment,
                           const MarketSeries &series) {
  const int count = series.count;
  const int xMax = segment.ticks[segment.tickCount - 1];
  if (count < 2 || xMax <= 0) return nullptr;

  chartRelease();
  gCanvas = new GFXcanvas1(EPD_WIDTH, EPD_HEIGHT);
  if (gCanvas == nullptr || gCanvas->getBuffer() == nullptr) {
    chartRelease();
    return nullptr;
  }
  GFXcanvas1 &canvas = *gCanvas;
  canvas.fillScreen(PAPER);
  canvas.setTextSize(1);
  canvas.setTextColor(INK, PAPER);

  // ---- header -------------------------------------------------------------
  canvas.setFont(&FreeSansBold9pt7b);
  drawText(canvas, TEXT_LEFT, TITLE_TOP, segment.displayName);

  canvas.setFont(&FreeSansBold18pt7b);
  char priceText[32];
  formatPrice(series.price, priceText, sizeof(priceText));
  drawText(canvas, TEXT_LEFT, PRICE_TOP, priceText);

  const float pct =
      (series.price - series.prevClose) / series.prevClose * 100.0f;
  char badgeText[24];
  snprintf(badgeText, sizeof(badgeText), "%+.2f%%", pct);
  canvas.setFont(&FreeSansBold9pt7b);

  int16_t boundsX, boundsY;
  uint16_t textWidth, textHeight;
  canvas.getTextBounds(badgeText, 0, 0, &boundsX, &boundsY, &textWidth,
                       &textHeight);
  const int boxWidth = textWidth + 2 * BADGE_PAD_X;
  const int boxHeight = textHeight + 2 * BADGE_PAD_Y;
  const int boxX = (int)lroundf(BADGE_RIGHT) - boxWidth;
  const int boxY = (int)lroundf(BADGE_CENTER_Y - boxHeight / 2.0f);
  canvas.fillRoundRect(boxX, boxY, boxWidth, boxHeight, 4, INK);
  canvas.setTextColor(PAPER, INK);  // white text punched out of the badge
  canvas.setCursor(boxX + BADGE_PAD_X - boundsX, boxY + BADGE_PAD_Y - boundsY);
  canvas.print(badgeText);
  canvas.setTextColor(INK, PAPER);

  // ---- chart --------------------------------------------------------------
  float yMin = series.prevClose, yMax = series.prevClose;
  for (int i = 0; i < count; ++i) {
    yMin = fminf(yMin, series.closes[i]);
    yMax = fmaxf(yMax, series.closes[i]);
  }
  float margin = (yMax - yMin) * 0.1f;
  if (margin <= 0.0f) margin = 0.001f;
  const float yLo = yMin - margin;
  const float yHi = yMax + margin;
  const float baselineY = chartY(series.prevClose, yLo, yHi);

  const int chartTop = (int)CHART_TOP;
  const int chartBottom = (int)CHART_BOTTOM;

  // matplotlib's dotted baseline, spanning the full axes width.
  for (int x = (int)CHART_X0; x <= (int)CHART_X1; x += 3) {
    canvas.drawPixel(x, (int)lroundf(baselineY), INK);
  }

  // Grey level per row: darkest at the far end of the axes, lightest next to
  // the baseline, exactly like the server's fading fill.
  uint8_t grey[EPD_HEIGHT];
  for (int y = chartTop; y <= chartBottom; ++y) {
    const float value =
        yHi - (float)(y - chartTop) / (float)(chartBottom - chartTop) *
                  (yHi - yLo);
    float t;
    if (value >= series.prevClose) {
      const float span = yHi - series.prevClose;
      t = span > 0.0f ? (value - series.prevClose) / span : 0.0f;
    } else {
      const float span = series.prevClose - yLo;
      t = span > 0.0f ? (series.prevClose - value) / span : 0.0f;
    }
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    grey[y] =
        (uint8_t)lroundf(FILL_LIGHT + (FILL_DARK - FILL_LIGHT) * t);
  }

  // Fill between the curve and the baseline, clipped to where the data ends.
  for (int x = (int)CHART_X0; x <= (int)CHART_X1; ++x) {
    const float position =
        ((float)x - CHART_X0) / (CHART_X1 - CHART_X0) * (float)xMax;
    if (position < 0.0f || position > (float)(count - 1)) continue;

    const int index = (int)position;
    const int next = index + 1 < count ? index + 1 : index;
    const float fraction = position - (float)index;
    const float value = series.closes[index] +
                        (series.closes[next] - series.closes[index]) * fraction;
    const float curveY = chartY(value, yLo, yHi);

    int from = (int)lroundf(fminf(curveY, baselineY));
    int to = (int)lroundf(fmaxf(curveY, baselineY));
    if (from < chartTop) from = chartTop;
    if (to > chartBottom) to = chartBottom;

    for (int y = from; y <= to; ++y) {
      if (grey[y] < BAYER4[y & 3][x & 3]) canvas.drawPixel(x, y, INK);
    }
  }

  // The session line itself, two pixels wide like matplotlib's linewidth 2.
  for (int i = 1; i < count; ++i) {
    const int x0 = (int)lroundf(chartX(i - 1, xMax));
    const int x1 = (int)lroundf(chartX(i, xMax));
    const int y0 = (int)lroundf(chartY(series.closes[i - 1], yLo, yHi));
    const int y1 = (int)lroundf(chartY(series.closes[i], yLo, yHi));
    canvas.drawLine(x0, y0, x1, y1, INK);
    canvas.drawLine(x0, y0 + 1, x1, y1 + 1, INK);
  }

  // ---- tick labels --------------------------------------------------------
  // The built-in 5x7 font is the closest match to the server's 7pt labels.
  canvas.setFont(nullptr);
  for (int i = 0; i < segment.tickCount; ++i) {
    if (segment.ticks[i] > xMax) continue;
    drawTextCentered(canvas, chartX(segment.ticks[i], xMax), TICK_LABEL_TOP,
                     segment.tickLabels[i]);
  }

  return canvas.getBuffer();
}

void chartRelease() {
  delete gCanvas;
  gCanvas = nullptr;
}
