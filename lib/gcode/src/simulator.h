// The modal simulator (sim/simulator.ts), one line at a time. Internal to the library.
//
// TS keeps every line's position in an array and rescales the stored values when the working
// precision rises. Streaming, the simulator keeps only the modal state; it records each rise
// (line, factor) so that a later pass can bring any line's values to the final precision with
// the same multiplications, in the same order (15 §4.4, §5.1).
#pragma once

#include <cstddef>
#include <cstdint>

#include "gcode/analysis.h"

namespace gcode {

class Simulator {
 public:
  /** A precision rise at `line`: values of earlier lines are multiplied by `factor`. */
  struct Raise {
    uint32_t line;
    double factor;
  };

  Simulator() { reset(); }
  void reset();

  /**
   * Simulates one line (0-based `index`, lines given in order). Fills `st` with the line's
   * state; `st.after` is at the working precision `st.precision`, and `st.before` is the
   * previous line's `after` brought to that precision.
   */
  void step(const char* text, size_t length, uint32_t index, LineState& st);

  /** After the last line: extents, display extents and the units summary (simulate()'s result). */
  void finish(Extents& extents, Extents& display, Units& units, bool& assumed, bool& mixed) const;

  int precision() const { return q_; }
  int32_t firstXYMotion() const { return firstXYMotion_; }
  const Raise* raises() const { return raises_; }
  int raiseCount() const { return raiseCount_; }

 private:
  class LineSink;
  void raisePrecision(int to, uint32_t line);
  /** One G-code of a line, in order: the modal loop of simulate(). */
  void applyG(int key, int& nonModal);

  // Modal state (§6), as indices into the TS tuples.
  int motion_, plane_, distance_, arcDistance_, units_, wcs_, feed_;
  bool motionSet_, distanceSet_, unitsSet_;
  int firstExplicitUnits_;  // -1, 0 (in), 1 (mm)
  bool assumedMotion_, assumedDistance_, assumedUnits_;

  int q_;
  double x_, y_, z_;
  bool everKnownXY_;
  int32_t firstXYMotion_;
  int primary_;  // -1, 0 (in), 1 (mm)
  bool mixed_;
  double exact_[4], arcExact_[4], display_[4];  // minX, maxX, minY, maxY

  Raise raises_[16];
  int raiseCount_;
};

/** Applies the rises after `line` to a value of that line (TS's rescaling of stored values). */
double toFinal(double v, uint32_t line, const Simulator::Raise* raises, int count, bool& overflow);

}  // namespace gcode
