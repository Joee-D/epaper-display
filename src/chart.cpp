#include "chart.h"

#include <Adafruit_GFX.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>
#include <string.h>

#include "display.h"

namespace {

// GFXcanvas1 sets a bit for any non-zero colour and GxEPD2::drawBitmap() paints
// a set bit in the colour it is given, so 1 = ink keeps the framebuffer and the
// panel driver in one convention.
constexpr uint16_t INK = 1;
constexpr uint16_t PAPER = 0;

// Geometry measured from the server-side matplotlib figure that main.py used to
// render (figsize 4x3 at dpi 100, gridspec height_ratios [1, 2.2],
// tight_layout(pad=0.2)); the same layout is kept here.
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

// Brightness below which a pixel is burnt to ink.
constexpr float INK_THRESHOLD = 128.0f;

// Per-column fill span plus the two error rows of the diffusion pass. Static
// rather than automatic because the Arduino loop task only has an 8 KB stack.
struct FillScratch {
  int16_t spanTop[EPD_WIDTH];
  int16_t spanBottom[EPD_WIDTH];
  float carried[EPD_WIDTH + 2];
  float pending[EPD_WIDTH + 2];
} scratch;

// Axis mapping for one chart, plus the price range the axes covers.
struct Plot {
  float xMax;  // index of the last tick: the right edge of the axes
  float yLo, yHi;

  float x(float index) const {
    return CHART_X0 + index / xMax * (CHART_X1 - CHART_X0);
  }
  float y(float price) const {
    return CHART_TOP + (yHi - price) / (yHi - yLo) * (CHART_BOTTOM - CHART_TOP);
  }
  float priceAtRow(float row) const {  // inverse of y(), for the fill ramp
    return yHi - (row - CHART_TOP) / (CHART_BOTTOM - CHART_TOP) * (yHi - yLo);
  }
};

// The data plus the previous close, with the 10% headroom the figure used.
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

// "29843.25": plain two-decimal price, no thousands separator.
void formatPrice(float value, char *out, size_t size) {
  const long long cents = llroundf(fabsf(value) * 100.0f);
  snprintf(out, size, "%s%lld.%02d", value < 0.0f ? "-" : "", cents / 100,
           (int)(cents % 100));
}

// Ink box of a string, relative to the cursor. getTextBounds() measures it the
// same way the renderer draws it, so positioning by the box removes any
// guesswork about where a font measures from.
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

// Title, price and the right-aligned change badge.
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

  canvas.setFont(&FreeSansBold9pt7b);
  const TextBox box = textBox(canvas, badge);
  const int width = box.width + 2 * BADGE_PAD_X;
  const int height = box.height + 2 * BADGE_PAD_Y;
  const int left = (int)lroundf(BADGE_RIGHT) - width;
  const int top = (int)lroundf(BADGE_CENTER_Y - height / 2.0f);
  canvas.fillRoundRect(left, top, width, height, 4, INK);
  canvas.setTextColor(PAPER, INK);  // white text punched out of the badge
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

  // ---- chart --------------------------------------------------------------
  const Plot plot = makePlot(segment, series);
  const float baselineY = plot.y(series.prevClose);
  const int chartTop = (int)CHART_TOP;
  const int chartBottom = (int)CHART_BOTTOM;

  // matplotlib's dotted baseline, spanning the full axes width.
  for (int x = (int)CHART_X0; x <= (int)CHART_X1; x += 3) {
    canvas.drawPixel(x, (int)lroundf(baselineY), INK);
  }

  // Grey level per row: lightest next to the baseline, darkest at the far end of
  // the axes, exactly like the fading fill in the figure.
  uint8_t grey[EPD_HEIGHT];
  for (int row = chartTop; row <= chartBottom; ++row) {
    const float price = plot.priceAtRow(row);
    const float span = price >= series.prevClose ? plot.yHi - series.prevClose
                                                : series.prevClose - plot.yLo;
    float t = span > 0.0f ? fabsf(price - series.prevClose) / span : 0.0f;
    t = t > 1.0f ? 1.0f : t;
    grey[row] = (uint8_t)lroundf(FILL_LIGHT + (FILL_DARK - FILL_LIGHT) * t);
  }

  // Fill between the curve and the baseline. The per-column span is worked out
  // first so the diffusion pass below can run in raster order.
  memset(&scratch, 0, sizeof(scratch));
  for (int x = 0; x < EPD_WIDTH; ++x) {
    scratch.spanTop[x] = 1;
    scratch.spanBottom[x] = 0;  // empty span until proven otherwise

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

  // Floyd-Steinberg error diffusion over the chart area, the same dithering the
  // image server applied. An ordered matrix only offers a handful of density
  // steps, which showed up as banding across a gradient this tall.
  {
    float *carried = scratch.carried;
    float *pending = scratch.pending;
    for (int y = chartTop; y <= chartBottom; ++y) {
      for (int x = 0; x < EPD_WIDTH; ++x) {
        const bool inFill = y >= scratch.spanTop[x] && y <= scratch.spanBottom[x];
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

  // The session line itself, two pixels wide like matplotlib's linewidth 2.
  for (int i = 1; i < count; ++i) {
    const int x0 = (int)lroundf(plot.x(i - 1));
    const int x1 = (int)lroundf(plot.x(i));
    const int y0 = (int)lroundf(plot.y(series.closes[i - 1]));
    const int y1 = (int)lroundf(plot.y(series.closes[i]));
    canvas.drawLine(x0, y0, x1, y1, INK);
    canvas.drawLine(x0, y0 + 1, x1, y1 + 1, INK);
  }

  // ---- tick labels --------------------------------------------------------
  // The built-in 5x7 font is the closest match to the server's 7pt labels.
  canvas.setFont(nullptr);
  for (int i = 0; i < segment.tickCount; ++i) {
    drawTextCentered(canvas, plot.x(segment.ticks[i]), TICK_LABEL_TOP,
                     segment.tickLabels[i]);
  }

  return displayShowBitmap(canvas.getBuffer());
}
