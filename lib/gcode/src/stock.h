// Stock comments (ops/stock.ts). Internal to the library.
#pragma once

#include "arena.h"
#include "gcode/analysis.h"

namespace gcode {

/** stockCommentsIn: appends the line's valid stock comments to `out`; false when out of memory. */
bool stockCommentsIn(const char* text, size_t length, uint32_t line, ArenaAlloc& arena, Vec<StockComment>& out);

/** stockBox: the comment's XY box at precision p; false when a bound isn't a safe integer at p. */
bool stockBox(const StockComment& c, int p, Extents& box);

}  // namespace gcode
