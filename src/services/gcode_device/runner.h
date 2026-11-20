// The device test suite's runner (specs/15 §9.4). No platform code, no exceptions, no standard
// library containers: the display builds it into a test firmware over its SD card, and CI builds
// it on the host over plain files. It reads the suite's data (data/manifest.tsv, the vectors, the
// cases' inputs and expected results), runs each case through the library, and prints one result
// line per case, then a summary.
#pragma once

#include <cstddef>
#include <cstdint>

#include "gcode/io.h"

namespace gcode_device {

/** What the runner needs from the platform (implemented by the caller). */
class Platform {
 public:
  /** Opens a data file for reading line by line; null if it can't. `path` is relative to the data folder. */
  virtual gcode::LineReader* open(const char* path) = 0;
  virtual void close(gcode::LineReader* reader) = 0;
  /** Opens a scratch file for writing (the perf group's write rate); null to skip that measurement. */
  virtual gcode::ByteWriter* create(const char* path) { (void)path; return nullptr; }
  virtual void closeWriter(gcode::ByteWriter* writer) { (void)writer; }
  /** Writes one result line (no newline in `text`) to the results file and the serial port. */
  virtual void print(const char* text) = 0;
  /** A millisecond clock, for the perf group. */
  virtual uint32_t millis() = 0;

 protected:
  ~Platform() = default;
};

struct Totals {
  uint32_t passed, failed, skipped;
};

/**
 * Runs the suite. The runner keeps no static buffers: it takes about 17 KB of scratch from the
 * front of `arena`, and the rest is the working memory for each analysis (reused case by case);
 * a case needing more is SKIPped with the reason. `groups` is a comma-separated filter
 * ("float,analyse"), or null for all groups.
 *
 * The run never yields: the platform's LineReader should pause regularly on an RTOS (docs/guide.md
 * §4).
 */
Totals run(Platform& platform, gcode::Arena arena, const char* groups);

}  // namespace gcode_device
