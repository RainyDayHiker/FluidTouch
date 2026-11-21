// Stock comments (ops/stock.ts). Internal to the library.
#pragma once

#include "arena.h"
#include "gcode/analysis.h"

namespace gcode {

/** Receives a line's valid stock comments, in order; their value texts point into the line. */
class StockVisitor {
 public:
  /** One comment; false stops the scan. */
  virtual bool comment(const StockComment& c) = 0;

 protected:
  ~StockVisitor() = default;
};

/** stockCommentsIn without copying: each valid stock comment of the line, in order. False if `out` stopped. */
bool eachStockComment(const char* text, size_t length, uint32_t line, StockVisitor& out);

/** stockCommentsIn: appends the line's valid stock comments to `out`; false when out of memory. */
bool stockCommentsIn(const char* text, size_t length, uint32_t line, ArenaAlloc& arena, Vec<StockComment>& out);

/** stockBox: the comment's XY box at precision p; false when a bound isn't a safe integer at p. */
bool stockBox(const StockComment& c, int p, Extents& box);

}  // namespace gcode
