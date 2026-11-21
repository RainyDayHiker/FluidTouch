// The Outline Check inside analyse() (specs/15 §18.2). Internal to the library.
#pragma once

#include "arena.h"
#include "gcode/analysis.h"
#include "gcode/outline.h"

namespace gcode {

/** The fill's fixed stack (15 §18.3): 1,024 seeds, 4 KB. */
constexpr uint32_t kOutlineFillSeeds = 1024;

/** Knobs and counters for the unit tests. */
struct OutlineTuning {
  /** Seeds the fill's stack holds (a small one exercises the sweeps). */
  uint32_t fillSeeds = kOutlineFillSeeds;
  /** Out: sweeps the fill's fallback made. */
  uint32_t sweeps = 0;
  /** Out: where the outline stopped, if it did: 1 Setup, 2 pass 2, 3 Finish; 0 it didn't. */
  uint8_t stoppedIn = 0;
};

/**
 * analyse() with the outline (AnalysisOptions::outline, whatever `options` says): the analysis,
 * then Finish, then result's outline fields. `reportOnly` builds the report without the grid.
 */
Status analyseOutline(LineReader& reader, const AnalysisOptions& options, ArenaAlloc& arena, AnalysisSink* sink, Analysis& result,
                      bool reportOnly, OutlineTuning& tuning);

/** outlineCheck() with the tuning. */
Status outlineCheckTuned(LineReader& reader, ByteWriter& writer, const char* sourceName, size_t nameLength, const OutlineOptions& options,
                         Arena arena, Report& report, OutlineResult& result, OutlineTuning& tuning);

}  // namespace gcode
