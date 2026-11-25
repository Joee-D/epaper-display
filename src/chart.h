#pragma once
#include <stdint.h>

#include "market.h"

// Draws the chart for `segment` and pushes it to the panel, skipping the redraw
// when the frame is identical to the one already shown.
bool chartDisplay(const MarketSegment &segment, const MarketSeries &series);
