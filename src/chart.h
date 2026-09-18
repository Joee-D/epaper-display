#pragma once
#include <stdint.h>

#include "market.h"

// Draws the chart into a 400x300 1bpp buffer (bit set = black), the exact
// format the panel consumes, laid out like the server-side matplotlib figure.
// Returns nullptr when the buffer cannot be allocated; the buffer stays valid
// until chartRelease().
const uint8_t *chartRender(const MarketSegment &segment, const MarketSeries &series);

// Frees the buffer handed out by chartRender().
void chartRelease();
