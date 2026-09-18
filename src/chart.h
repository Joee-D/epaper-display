#pragma once
#include <stdint.h>

#include "market.h"

// Draws the chart for `segment` and pushes it to the panel, skipping the redraw
// when the frame is identical to the one already shown. Returns false when the
// frame could not be allocated or the segment has too little data.
bool chartDisplay(const MarketSegment &segment, const MarketSeries &series);
