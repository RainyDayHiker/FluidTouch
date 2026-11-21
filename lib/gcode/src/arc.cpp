// Arc interpretation for G2/G3 in G17, following Grbl (specs/04 §8): sim/arc.ts line for line.
#include "arc.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

#include "gcode/num.h"

namespace gcode {
namespace {

const double ARC_EPSILON = 5e-7;
const double PI = 3.141592653589793;
const double TWO_PI = 2 * PI;
struct Cardinal {
  double phi;
  double dx, dy;
};
const Cardinal CARDINALS[4] = {
    {0, 1, 0},
    {PI / 2, 0, 1},
    {PI, -1, 0},
    {(3 * PI) / 2, 0, -1},
};

/** Appends text to the reason. */
struct Reason {
  ArcInfo& info;
  void put(const char* s) {
    while (*s && info.reasonLength < sizeof(info.reason)) info.reason[info.reasonLength++] = *s++;
  }
  /** fmt(n): formatDecimal(n, 4). */
  void num(double n) {
    char buf[48];
    const int len = formatDecimal(n, 4, false, buf, sizeof buf);
    for (int k = 0; k < len && info.reasonLength < sizeof(info.reason); k++) info.reason[info.reasonLength++] = buf[k];
  }
};

bool fail(ArcInfo& out, const char* reason) {
  out.state = ArcState::Invalid;
  out.reasonLength = 0;
  Reason{out}.put(reason);
  return false;
}

bool centreFromIJ(const ArcInput& a, double& cx, double& cy, double& r, ArcInfo& out) {
  if (!a.hasI && !a.hasJ) return fail(out, "arc has no centre (I/J) or radius (R)");
  cx = a.sx + (a.hasI ? a.i : 0);
  cy = a.sy + (a.hasJ ? a.j : 0);
  r = fmath::hypot(a.sx - cx, a.sy - cy);
  if (r == 0) return fail(out, "arc radius is zero");
  const double dr = std::fabs(fmath::hypot(a.ex - cx, a.ey - cy) - r);
  const double k = a.inches ? 1 / 25.4 : 1;
  if (dr > 0.5 * k || (dr > 0.005 * k && dr > 0.001 * r)) {
    out.state = ArcState::Invalid;
    out.reasonLength = 0;
    Reason t{out};
    t.put("end point isn't on the arc (radius ");
    t.num(r);
    t.put(" at the start, ");
    t.num(r + dr);
    t.put(" or ");
    t.num(r - dr);
    t.put(" at the end)");
    return false;
  }
  return true;
}

bool centreFromR(const ArcInput& a, double rWord, double& cx, double& cy, double& r, ArcInfo& out) {
  if (rWord == 0) return fail(out, "R is zero");
  const double x = a.ex - a.sx;
  const double y = a.ey - a.sy;
  if (x == 0 && y == 0) return fail(out, "R form can't describe a full circle (start equals end)");
  double h2 = 4 * rWord * rWord - x * x - y * y;
  // A half circle whose chord came out a hair longer than 2|R| from rounding E - S is a half
  // circle (09 D71): within 2^-40 of the arc's largest coordinate, as arc.ts's R_ROUNDING.
  if (h2 < 0) {
    double scale = std::fabs(a.sx);
    for (const double v : {std::fabs(a.sy), std::fabs(a.ex), std::fabs(a.ey), std::fabs(rWord)}) scale = v > scale ? v : scale;
    if (!(fmath::hypot(x, y) - 2 * std::fabs(rWord) > scale * (1 / 1099511627776.0))) h2 = 0;
  }
  if (h2 < 0) {
    out.state = ArcState::Invalid;
    out.reasonLength = 0;
    Reason t{out};
    t.put("R ");
    t.num(std::fabs(rWord));
    t.put(" is too small for the distance between start and end");
    return false;
  }
  double h = -std::sqrt(h2) / fmath::hypot(x, y);
  if (!a.clockwise) h = -h;
  if (rWord < 0) h = -h;
  cx = a.sx + (x - y * h) / 2;
  cy = a.sy + (y + x * h) / 2;
  r = std::fabs(rWord);
  return true;
}

}  // namespace

void interpretArc(const ArcInput& a, ArcInfo& out) {
  double cx = 0, cy = 0, r = 0;
  const bool ok = a.rForm ? centreFromR(a, a.r, cx, cy, r, out) : centreFromIJ(a, cx, cy, r, out);
  if (!ok) return;
  const double startAngle = fmath::atan2(a.sy - cy, a.sx - cx);
  double sweep = fmath::atan2(a.ey - cy, a.ex - cx) - startAngle;
  if (a.clockwise) {
    if (sweep >= -ARC_EPSILON) sweep -= TWO_PI;
  } else if (sweep <= ARC_EPSILON) {
    sweep += TWO_PI;
  }
  ArcGeometry& g = out.geometry;
  g.extremeCount = 0;
  const double span = std::fabs(sweep);
  for (const Cardinal& c : CARDINALS) {
    double t = std::fmod(a.clockwise ? startAngle - c.phi : c.phi - startAngle, TWO_PI);
    if (t < 0) t += TWO_PI;
    if (t > 0 && t < span) {
      g.extremeX[g.extremeCount] = cx + c.dx * r;
      g.extremeY[g.extremeCount] = cy + c.dy * r;
      g.extremeCount++;
    }
  }
  out.state = ArcState::Ok;
  out.reasonLength = 0;
  g.cx = cx;
  g.cy = cy;
  g.r = r;
  g.startAngle = startAngle;
  g.sweep = sweep;
}

}  // namespace gcode
