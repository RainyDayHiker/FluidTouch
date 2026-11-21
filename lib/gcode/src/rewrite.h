// Rewriting a line's X/Y/I/J words and stock bounds (ops/rotate90.ts §"Rewriting"): the transform,
// its setup, and the edits of one line. Internal to the library.
#pragma once

#include <cstddef>
#include <cstdint>

#include "gcode/analysis.h"
#include "gcode/transform.h"

namespace gcode {

/** rotate90.ts Transform, with what setupTransform() works out once (06 §1, §4). */
struct TransformSetup {
  bool mirror;     ///< Flip: each word keeps its letter, one axis mirrors, G2/G3 swap.
  bool mirrorX;    ///< The mirrored axis is X (Flip H); else Y.
  double sum;      ///< min + max of the mirrored axis.
  bool clockwise;  ///< For a rotation.
  double cx, cy;   ///< The rotation's constants.
  int q;           ///< Working precision: the document's, or more for the stock's digits.
};

/**
 * setupTransform: null (false) when the file has no extents. `hasG10`: some line has a G10.
 * The pivot is the last stock comment's box when it fits at q, else the job's box.
 */
bool setupTransform(TransformKind kind, const Analysis& a, bool hasG10, TransformSetup& out);

/** One replacement in a line: bytes [start, end) become `text`. */
struct Edit {
  uint32_t start, end;
  char text[64];
  uint8_t length;
};

/**
 * The X/Y/I/J (and, for a mirror, G2/G3) replacements of a line (computeReps), in line order:
 * at most five. False when a value isn't a safe integer (an overflow).
 */
bool wordEdits(const char* text, size_t length, const TransformSetup& t, bool incremental, Edit* out, int& count);

/** Receives a stock line's edits, in line order (rewriteStock). */
class EditSink {
 public:
  virtual bool edit(const Edit& e) = 0;

 protected:
  ~EditSink() = default;
};

/**
 * The stock-bound replacements of a line (rewriteStock), each comment's in order. False when a
 * bound overflows (stockFits) or `sink` stops. A null sink only checks.
 */
bool stockEdits(const char* text, size_t length, uint32_t line, const TransformSetup& t, EditSink* sink);

}  // namespace gcode
