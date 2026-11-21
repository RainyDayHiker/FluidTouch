// The operation structure of a FreeCAD-posted file (ops/structure.ts), found in one streaming
// pass (15 §6.2): the backward scans of TS become running state. Internal to the library.
#pragma once

#include "arena.h"
#include "gcode/analysis.h"

namespace gcode {

/** stripOff's prefixLength: the `;OFF ` or `;OFF[Tn] ` prefix of a line that's off, else 0. */
uint32_t offPrefixLength(const char* text, size_t length);

/** trimBlanks (parse/blanks.ts): the line without leading and trailing spaces and tabs. */
Str trimmedBlanks(const char* text, size_t length);

/** What a line is to Flip's cut groups (flip.ts cutGroups), by its trimmed text. */
enum class LineMarker : uint8_t {
  None,
  BeginOperation,   ///< `(Begin operation: NAME)`; the name is set.
  FinishOperation,  ///< `(Finish operation: …)`.
  ToolChange,       ///< `(Begin toolchange)` or an M6 line (M6_RE).
};

/** The line's marker; for BeginOperation, `name` is markerName's, pointing into `text`. */
LineMarker lineMarker(const char* text, size_t length, Str& name);

class StructureFinder {
 public:
  StructureFinder(ArenaAlloc& arena, char* scratch, size_t scratchSize) : arena_(arena), scratch_(scratch), scratchSize_(scratchSize) {}

  /** One line, in order (0-based index). */
  void step(const char* text, size_t length, uint32_t index);
  /** After the last line: fills `out` (findOperationStructure, then toolLabels). */
  void finish(Structure& out);

 private:
  struct Block {
    uint8_t kind;  // 0 op, 1 tc, 2 fixture
    Str name;
    uint32_t start, begin, end;
    bool off;
    bool hasTaggedTool;
    double taggedTool;
    bool hasTool;
    double tool;
    int32_t beginToolchange, m6;
    bool hasLabel;
    Str label;
  };
  struct ProbeHit {
    uint32_t count;
    uint32_t line;
    Str label;
    uint32_t labelStart, labelEnd;
  };

  void invalid(const char* reasonPrefix, Str name, const char* reasonSuffix, uint32_t index);
  void invalid2(const char* a, Str n1, const char* b, Str n2, const char* c, uint32_t index);

  ArenaAlloc& arena_;
  char* scratch_;
  size_t scratchSize_;

  bool done_ = false;  // an invalid structure was found
  Str reason_{"", 0};
  uint32_t reasonLine_ = 0;

  Vec<Block> blocks_;
  bool open_ = false;
  Block cur_{};
  uint32_t nonComment_ = 0, fixtureLines_ = 0;
  int64_t afterPrev_ = -1;
  int64_t firstNonComment_ = -1;  // first non-comment line since afterPrev
  uint32_t msgRun_ = 0;           // consecutive (MSG lines just before the current line
  ProbeHit beforeRun_{0, 0, {"", 0}, 0, 0}, inRun_{0, 0, {"", 0}, 0, 0};
  bool firstBlockSeen_ = false;
  uint32_t firstStart_ = 0;
  ProbeHit preambleProbes_{0, 0, {"", 0}, 0, 0};
};

}  // namespace gcode
