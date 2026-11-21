// Rotate 90° CW / CCW and Flip Horizontal / Vertical (specs/15 §5.3, C2): ops/rotate90.ts's
// makeTransform with the classification of ops/classify.ts, the reports of ops/types.ts, Flip's
// warnings of ops/flip.ts, and the enabled form of ops/enabled-form.ts, as the editor's menu runs
// them.
#pragma once

#include <cstddef>
#include <cstdint>

#include "gcode/analysis.h"
#include "gcode/io.h"

namespace gcode {

/** Which transform. */
enum class TransformKind : uint8_t {
  RotateCW,   ///< Rotate 90° clockwise (rotate90.cw).
  RotateCCW,  ///< Rotate 90° counterclockwise (rotate90.ccw).
  FlipH,      ///< Flip horizontally: X mirrors (flip.horizontal).
  FlipV,      ///< Flip vertically: Y mirrors (flip.vertical).
};

/** An issue's severity (ops/types.ts Severity). */
enum class Severity : uint8_t {
  Blocking,  ///< The transform can't run.
  Warning,   ///< The user confirms before it runs.
  Info,      ///< For information.
};

/** One issue of a report (ops/types.ts ValidationIssue). */
struct Issue {
  /** 1-based line. */
  uint32_t line;
  /** Byte offset of the line's text in the file (after a UTF-8 BOM), for reportLineText(). */
  uint64_t offset;
  /** Bytes of the line's text, without its ending. */
  uint32_t length;
  Severity severity;   ///< Blocking, warning or info.
  const char* ruleId;  ///< The stable rule id, e.g. "B-G92", "W2" (statuses.md lists them).
  /** The message: the TS text, byte for byte; file text inside it is the file's own bytes. */
  Str reason;
  /**
   * The warning is answered with a choice rather than Continue (W-FLIP): "Flip and reverse
   * cuts" (FlipChoice::FlipAndReverseCuts, recommended) or "Flip only" (FlipChoice::FlipOnly).
   */
  bool hasChoices;
};

/** A validation report (ops/types.ts ValidationReport). */
struct Report {
  /** At most kMaxReportedIssues: blocking first, then warnings, then info, each by line. */
  const Issue* issues;
  uint32_t count;    ///< Issues in `issues`.
  uint32_t omitted;  ///< Issues left out by the cap ("and N more…").
  bool blocked;      ///< Some issue, reported or omitted, is Blocking.
  bool warnings;     ///< Some issue, reported or omitted, is a warning.
  /** Bytes of the arena the call used (check, or apply with its check): size the arena from this. */
  size_t arenaUsed;
};

/** MAX_REPORTED_ISSUES. */
constexpr uint32_t kMaxReportedIssues = 200;

/** The answer to W-FLIP (12 §3). */
enum class FlipChoice : uint8_t {
  FlipOnly,            ///< Mirror the toolpath ("mirror").
  FlipAndReverseCuts,  ///< Mirror, then reverse the cuts ("reverse"): NotSupported until C4.
};

/** How a transform reads and reports progress. */
struct TransformOptions {
  /** Longest line accepted, in bytes (15 §4.2); at most 65,535. A longer line blocks. */
  uint32_t maxLineLength = 1024;
  /** Called every kCheckpointLines lines of each pass; may be null. */
  Progress progress = nullptr;
  /** Passed to progress. */
  void* progressContext = nullptr;
};

/** What apply() did (the TS OperationResult). */
struct TransformResult {
  /** The summary, e.g. "Rotated 90° CW · 1,234 of 5,678 lines changed"; UTF-8, terminated. */
  char summary[192];
  uint32_t summaryLength;  ///< Bytes of summary.
  uint32_t linesChanged;   ///< Lines whose text changed.
  uint32_t lineCount;      ///< Lines in the file.
};

/**
 * Validates a transform: the TS operation's validate(), on the file's enabled form (every
 * `;OFF ` prefix removed, 11 §6). Rewinds `reader` first, then two passes; writes nothing. `report` points into `arena`.
 * Status Ok with the report (which may block), or Refused, ReadError, OutOfMemory, Cancelled.
 * A line longer than options.maxLineLength gives a report with one Blocking issue,
 * B-LINE-TOO-LONG (the one rule the TS core doesn't have, 15 §4.2).
 */
Status check(LineReader& reader, TransformKind kind, const TransformOptions& options, Arena arena, Report& report);

/**
 * Applies a transform: check() again (the file may have changed since), then, unless it blocks,
 * one more pass writing every line through `writer` (TS apply() and serializeDocument()). Lines
 * that don't change are copied byte for byte. Returns Blocked with the report and writes nothing
 * when the report blocks; NotSupported, writing nothing, for FlipAndReverseCuts. Any other
 * status but Ok may leave a partial output, which the caller deletes (15 §4.5).
 */
Status apply(LineReader& reader, ByteWriter& writer, TransformKind kind, FlipChoice choice, const TransformOptions& options,
             Arena arena, Report& report, TransformResult& result);

/**
 * What a check() found, for applyChecked(): a handle to state check() left in its arena, with
 * `report`. Valid while the first `arenaUsed` bytes of that arena are left as they are.
 */
struct CheckedTransform {
  const void* state;  ///< Null when check() didn't return Ok.
  size_t arenaUsed;   ///< Bytes of the arena holding the state and the report.
};

/** check(), keeping what it found for applyChecked(). */
Status check(LineReader& reader, TransformKind kind, const TransformOptions& options, Arena arena, Report& report,
             CheckedTransform& checked);

/**
 * apply() without its second check (15 §5.3): the write pass alone, from what check() found.
 * The caller vouches that the file hasn't changed since that check(), as the editor's revision
 * check does; a file whose line count or size differs gives FileChanged (a cheap guard, not a
 * proof). Pass the same `arena` as check(): only the bytes past `checked.arenaUsed` are used, so
 * check()'s report stays valid. Blocked, writing nothing, when the report blocked; NotSupported
 * for FlipAndReverseCuts. The output is byte for byte apply()'s. Any other status but Ok may
 * leave a partial output, which the caller deletes (15 §4.5).
 */
Status applyChecked(LineReader& reader, ByteWriter& writer, const CheckedTransform& checked, FlipChoice choice,
                    const TransformOptions& options, Arena arena, TransformResult& result);

/**
 * Reads an issue's line text back from the file (15 §5.3): up to `capacity` bytes into `out`.
 * Returns the bytes written, or -1 if the reader fails or the line isn't found. Rewinds `reader`.
 */
int32_t reportLineText(LineReader& reader, const Issue& issue, char* out, size_t capacity);

}  // namespace gcode
