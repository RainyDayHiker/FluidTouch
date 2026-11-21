// The Outline Check (specs/15 §5.4, §18, C3): ops/outline-check.ts's validateOutlineCheck and
// buildOutlineCheck, and the file of specs/13 §3. The outline is built inside analyse()'s two
// passes: on its own with outlineCheck(), or with the preview's analysis (AnalysisOptions::outline),
// then shown with outlineLoops() and written with writeOutline(), neither reading the file again.
#pragma once

#include <cstddef>
#include <cstdint>

#include "gcode/analysis.h"
#include "gcode/io.h"
#include "gcode/transform.h"

namespace gcode {

/** How the Outline Check reads and reports progress. */
struct OutlineOptions {
  /** Longest line accepted, in bytes (15 §4.2); at most 65,535. A longer line blocks (B-LINE-TOO-LONG). */
  uint32_t maxLineLength = 1024;
  /**
   * Called every kCheckpointLines lines of each pass, and in Finish (15 §18.2): as it starts,
   * after each sweep of the fill's fallback, every 65,536 cells traced and after each loop, with
   * the file's line count and size. May be null; returning false cancels.
   */
  Progress progress = nullptr;
  /** Passed to progress. */
  void* progressContext = nullptr;
};

/** The outline (the TS OutlineCheck without its text). Points into the arena. */
struct OutlineResult {
  /** No cutting moves: nothing to write, and the summary is "No cutting moves found". */
  bool empty;
  /** x, y pairs, fixed point at `precision`, loop after loop, each from the corner written first. */
  const double* corners;
  /** loopCount + 1 offsets into corners, in pairs: loop k is corners [loopStarts[k], loopStarts[k + 1]). */
  const uint32_t* loopStarts;
  uint32_t loopCount;  ///< How many loops.
  uint8_t precision;   ///< The scale of the corners: 10^precision.
  /** How close the outline is to the path, in the file's units: two cells, two significant digits. */
  double tolerance;
  /** "1 outline, 12 points, within 1 mm · X-3.799–46.201 Y2–30 mm"; UTF-8, terminated. */
  char summary[192];
  uint32_t summaryLength;  ///< Bytes of summary.
};

/**
 * The Outline Check's report (validateOutlineCheck, 13 §4), alone: analyses the file (two passes,
 * from its first line) and draws nothing. `report` points into `arena`. Ok with the report (which
 * may block), or Refused, ReadError, OutOfMemory, Cancelled. A line longer than
 * options.maxLineLength gives a report with one Blocking issue, B-LINE-TOO-LONG.
 */
Status checkOutline(LineReader& reader, const OutlineOptions& options, Arena arena, Report& report);

/**
 * The Outline Check on its own: analyses the file with the outline (two passes), then, unless the
 * report blocks, writes the file of 13 §3 through `writer`. `sourceName` (UTF-8) goes in its header
 * comment. Ok when written, or with result.empty when nothing cuts (nothing written); Blocked with
 * the report, writing nothing; Refused, ReadError, OutOfMemory or Cancelled, writing nothing;
 * NotSupported, writing nothing, when a corner isn't a safe integer (where TS throws); WriteError,
 * which may leave a partial file the caller deletes. `report` and `result` point into `arena`.
 */
Status outlineCheck(LineReader& reader, ByteWriter& writer, const char* sourceName, size_t nameLength, const OutlineOptions& options,
                    Arena arena, Report& report, OutlineResult& result);

/**
 * The report of an outline built by analyse() with AnalysisOptions::outline. Returns
 * Analysis::outlineStatus: Ok with the report, or why there is none. Valid while the arena
 * analyse() had isn't reused.
 */
Status outlineReport(const Analysis& analysis, Report& report);

/**
 * The loops and summary of an outline built by analyse() with AnalysisOptions::outline, writing
 * nothing. Blocked, with no loops, when the report blocks; Analysis::outlineStatus when there is
 * no outline. Valid while the arena analyse() had isn't reused.
 */
Status outlineLoops(const Analysis& analysis, OutlineResult& result);

/**
 * Writes the file of 13 §3 for an outline built by analyse() with AnalysisOptions::outline, without
 * reading the file. `arena` is the one analyse() had: the 4 KB write buffer comes from the free
 * space between the analysis and the outline it holds (OutOfMemory, writing nothing, without it).
 * Statuses as outlineLoops(), plus NotSupported and WriteError as outlineCheck().
 */
Status writeOutline(const Analysis& analysis, ByteWriter& writer, const char* sourceName, size_t nameLength, Arena arena,
                    OutlineResult& result);

/**
 * The outline file's suggested name (outlineCheckName): "part.nc" gives "part.outline.nc", a name
 * with no extension (or only a leading dot) gets ".outline.nc". UTF-8 in and out. Returns the new
 * name's length in bytes; writes it to `out`, terminated, only when `capacity` exceeds that.
 */
size_t outlineCheckName(const char* name, size_t length, char* out, size_t capacity);

}  // namespace gcode
