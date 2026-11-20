// Arc interpretation for G2/G3 in G17 (sim/arc.ts). Internal to the library.
#pragma once

#include "gcode/analysis.h"

namespace gcode {

struct ArcInput {
  bool clockwise;
  double sx, sy, ex, ey;
  /** Centre form: I/J (either may be absent) or R. */
  bool rForm;
  bool hasI, hasJ;
  double i, j, r;
  /** Radius tolerances in inches (G20). */
  bool inches;
};

/** interpretArc: fills `out` with Ok and the geometry, or Invalid and the reason. */
void interpretArc(const ArcInput& a, ArcInfo& out);

}  // namespace gcode
