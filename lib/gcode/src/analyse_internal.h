// analyse() with a hook for the transforms (specs/15 §5.3, §6.2). Internal to the library.
//
// A transform needs what pass 1 finds (precision, extents, job extents, fixed moves, stock
// comments) before it can look at any line, and then every line's state at the final precision.
// So it runs as a FinalPass inside analyse(): begin() after pass 1, line() for each line of pass 2.
#pragma once

#include "arena.h"
#include "gcode/analysis.h"

namespace gcode {

/** Facts from pass 1 that the Analysis doesn't carry. */
struct PassOneFacts {
  /** Every line's G-code bits, ORed (rotate90.ts hasG10 reads them). */
  uint32_t gLo;
  uint32_t gHi;  ///< The rest of the G-code bits, ORed.
};

class FinalPass {
 public:
  /** After pass 1: `a` holds everything but the per-pass-2 results. False stops (out of memory). */
  virtual bool begin(const Analysis& a, const PassOneFacts& facts) = 0;
  /** Each line of pass 2, as AnalysisSink::line(). False stops (out of memory). */
  virtual bool line(const Line& line, const Token* tokens, uint16_t tokenCount, const LineState& state) = 0;

 protected:
  ~FinalPass() = default;
};

/** analyse(), allocating from `arena` (shared with the caller), with an optional FinalPass. */
Status analyseWith(LineReader& reader, const AnalysisOptions& options, ArenaAlloc& arena, AnalysisSink* sink, FinalPass* pass,
                   Analysis& result);

}  // namespace gcode
