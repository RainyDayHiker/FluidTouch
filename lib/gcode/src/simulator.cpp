// Modal simulator and extents (specs/04 §6–9): sim/simulator.ts, one line at a time.
#include "simulator.h"

#include <cmath>
#include <cstring>

#include "arc.h"
#include "gcode/num.h"
#include "gcode/tokens.h"
#include "vocabulary.h"

namespace gcode {
namespace {

const int FLOOR_MM = 3, FLOOR_IN = 4;
const int M_G0 = 0, M_G1 = 1, M_G2 = 2, M_G3 = 3, M_G80 = 8;
bool isProbe(int m) { return m >= 4 && m <= 7; }
bool isCanned(int m) { return m >= 9; }
bool known(double v) { return !std::isnan(v); }
const double INF = HUGE_VAL;

/** MOTION_INDEX by code key. */
int motionIndex(int key) {
  switch (key) {
    case 0: return 0;
    case 10: return 1;
    case 20: return 2;
    case 30: return 3;
    case 382: return 4;
    case 383: return 5;
    case 384: return 6;
    case 385: return 7;
    case 800: return 8;
    case 730: return 9;
    case 760: return 10;
    default:
      if (key >= 810 && key <= 890 && key % 10 == 0) return 11 + (key - 810) / 10;
      return -1;
  }
}

const uint32_t ARC_WORDS = (1u << 8) | (1u << 9) | (1u << 17);  // I, J, R
// M56 and M62–M65 own a P word (06 §5.3): their bits in M_CODE_ORDER.
uint32_t pOwnerM() {
  uint32_t m = 0;
  static const int kOwners[] = {560, 620, 630, 640, 650};
  for (int key : kOwners) m |= 1u << vocab::mIndex(key);
  return m;
}

void boxAdd(double* b, double x, double y) {
  if (x < b[0]) b[0] = x;
  if (x > b[1]) b[1] = x;
  if (y < b[2]) b[2] = y;
  if (y > b[3]) b[3] = y;
}
bool boxEmpty(const double* b) { return b[0] == INF; }
void boxClear(double* b) {
  b[0] = INF;
  b[1] = -INF;
  b[2] = INF;
  b[3] = -INF;
}

struct Word {
  uint32_t start, end;
  int fractionDigits;
};

/** JS Math.min / Math.max: NaN wins, and -0 is below +0 (std::fmin / fmax differ on both). */
double jsMin(double a, double b) {
  if (a != a || b != b) return NAN;
  if (a == 0 && b == 0) return std::signbit(a) ? a : b;
  return a < b ? a : b;
}
double jsMax(double a, double b) {
  if (a != a || b != b) return NAN;
  if (a == 0 && b == 0) return std::signbit(a) ? b : a;
  return a > b ? a : b;
}

}  // namespace

/**
 * The line's words, as simulate()'s sink collects them. G-codes go straight to the simulator's
 * modal state, in order: TS collects them and applies them after the scan, in the same order,
 * and nothing in between depends on them, so the result is the same with no limit on their number.
 */
class Simulator::LineSink final : public TokenSink {
 public:
  LineSink(Simulator& sim, const char* text) : sim(sim), text(text) {}
  Simulator& sim;
  const char* text;
  uint32_t present = 0, allLetters = 0, mMask = 0;
  uint32_t gLo = 0, gHi = 0;
  bool hasG2 = false, hasG3 = false, hasG4 = false;
  int nonModal = -1;
  Word slots[26];

  void word(uint32_t, uint32_t end, char letter, uint32_t valueStart, int fractionDigits) override {
    const int k = letter - 'A';
    if (k >= 0 && k < 26) allLetters |= 1u << k;
    if (letter == 'G') {
      const int key = codeKey(text + valueStart, end - valueStart);
      if (key == -1) return;
      hasG2 = hasG2 || key == 20;
      hasG3 = hasG3 || key == 30;
      hasG4 = hasG4 || key == 40;
      const int b = vocab::gIndex(key);
      if (b >= 0) {
        if (b < 32) gLo |= 1u << b;
        else gHi |= 1u << (b - 32);
      }
      sim.applyG(key, nonModal);
      return;
    }
    if (letter == 'M') {
      const int bit = vocab::mIndex(codeKey(text + valueStart, end - valueStart));
      if (bit >= 0) mMask |= 1u << bit;
      return;
    }
    if (present & (1u << k)) return;
    present |= 1u << k;
    slots[k] = {valueStart, end, fractionDigits};
  }
  void space(uint32_t, uint32_t) override {}
  void comment(uint32_t, uint32_t, CommentStyle) override {}
  void raw(uint32_t, uint32_t) override {}

  const Word* word(char letter) const {
    const int k = letter - 'A';
    return present & (1u << k) ? &slots[k] : nullptr;
  }
  bool has(char letter) const { return (present & (1u << (letter - 'A'))) != 0; }
};

double toFinal(double v, uint32_t line, const Simulator::Raise* raises, int count, bool& overflow) {
  for (int k = 0; k < count; k++) {
    if (raises[k].line <= line) continue;
    if (!known(v)) continue;
    v *= raises[k].factor;
    if (!isSafeInteger(v)) overflow = true;
  }
  return v;
}

void Simulator::reset() {
  motion_ = M_G0;
  plane_ = 0;
  distance_ = 0;
  arcDistance_ = 1;
  units_ = 1;
  wcs_ = 0;
  feed_ = 1;
  motionSet_ = distanceSet_ = unitsSet_ = false;
  firstExplicitUnits_ = -1;
  assumedMotion_ = assumedDistance_ = assumedUnits_ = false;
  q_ = FLOOR_MM;
  x_ = y_ = z_ = NAN;
  everKnownXY_ = false;
  firstXYMotion_ = -1;
  primary_ = -1;
  mixed_ = false;
  boxClear(exact_);
  boxClear(arcExact_);
  boxClear(display_);
  raiseCount_ = 0;
}

void Simulator::raisePrecision(int to, uint32_t line) {
  if (to <= q_) return;
  const double k = pow10(to - q_);
  if (raiseCount_ < 16) raises_[raiseCount_++] = {line, k};
  if (known(x_)) x_ *= k;
  if (known(y_)) y_ *= k;
  if (!boxEmpty(exact_)) {
    exact_[0] *= k;
    exact_[1] *= k;
    exact_[2] *= k;
    exact_[3] *= k;
  }
  q_ = to;
}

void Simulator::applyG(int c, int& nonModal) {
  // Modal groups first (Grbl applies them before motion on the same line).
  {
    const int m = motionIndex(c);
    if (m >= 0) {
      motion_ = m;
      motionSet_ = true;
      return;
    }
    switch (c) {
      case 170: case 180: case 190: plane_ = (c - 170) / 10; break;
      case 900: distance_ = 0; distanceSet_ = true; break;
      case 910: distance_ = 1; distanceSet_ = true; break;
      case 901: arcDistance_ = 0; break;
      case 911: arcDistance_ = 1; break;
      case 930: case 940: case 950: feed_ = (c - 930) / 10; break;
      case 540: case 550: case 560: case 570: case 580: case 590: wcs_ = (c - 530) / 10; break;
      case 200: case 210: {
        const int next = c == 200 ? 0 : 1;
        unitsSet_ = true;
        if (firstExplicitUnits_ < 0) firstExplicitUnits_ = next;
        if (next != units_) {
          const double f = next == 0 ? 1 / 25.4 : 25.4;
          if (known(x_)) x_ = toFixed(toNumber(x_, q_) * f, q_);
          if (known(y_)) y_ = toFixed(toNumber(y_, q_) * f, q_);
          if (known(z_)) z_ *= f;
          units_ = next;
        }
        break;
      }
      case 40: case 100: case 280: case 281: case 300: case 301: case 530:
      case 920: case 921: case 922: case 923:
        nonModal = c;
        break;
      default:
        break;
    }
  }
}

void Simulator::step(const char* text, size_t length, uint32_t i, LineState& st) {
  uint32_t flags = 0;
  // `before` is the previous line's stored position, taken before this line's G20/G21 converts it.
  const double prevX = x_, prevY = y_, prevZ = z_;
  LineSink s(*this, text);
  scanLine(text, length, s);
  const int nonModal = s.nonModal;

  const Word* wx = s.word('X');
  const Word* wy = s.word('Y');
  const Word* wz = s.word('Z');
  const bool hasAxis = wx || wy || wz;
  Owner owner = Owner::None;
  if (hasAxis) {
    if (nonModal == 100) owner = Owner::G10;
    else if (nonModal == 280) owner = Owner::G28;
    else if (nonModal == 300) owner = Owner::G30;
    else if (nonModal == 920) owner = Owner::G92;
    else if (nonModal == 530) owner = Owner::G53;
    else if (isProbe(motion_)) owner = Owner::G38;
    else owner = Owner::Motion;
  }

  const bool motionLine = owner == Owner::Motion ||
                          (!hasAxis && nonModal < 0 && (motion_ == M_G2 || motion_ == M_G3) && (s.has('I') || s.has('J') || s.has('R')));
  const bool isXYMove = motionLine && motion_ <= M_G3 && ((wx || wy) || motion_ >= M_G2);

  if (owner == Owner::Motion && !motionSet_ && !assumedMotion_) {
    flags |= kMotionModeAssumed;
    assumedMotion_ = true;
  }
  if (isXYMove) {
    if (firstXYMotion_ < 0) firstXYMotion_ = static_cast<int32_t>(i);
    if (!distanceSet_ && !assumedDistance_) {
      flags |= kDistanceModeAssumed;
      assumedDistance_ = true;
    }
    if (!unitsSet_ && !assumedUnits_) {
      flags |= kUnitsAssumed;
      assumedUnits_ = true;
    }
    if (primary_ < 0) {
      primary_ = units_;
      if (primary_ == 0) raisePrecision(FLOOR_IN, i);
    } else if (units_ != primary_) {
      mixed_ = true;
    }
  }

  // Values. X/Y/I/J/R are fixed-point at q, which first rises to fit them.
  int need = q_;
  if (wx && wx->fractionDigits > need) need = wx->fractionDigits;
  if (wy && wy->fractionDigits > need) need = wy->fractionDigits;
  if (s.present & ARC_WORDS) {
    static const char kArcLetters[] = {'I', 'J', 'R'};
    for (char l : kArcLetters) {
      const Word* w = s.word(l);
      if (w && w->fractionDigits > need) need = w->fractionDigits;
    }
  }
  raisePrecision(need, i);
  const double startX = x_, startY = y_;
  const auto fixedOf = [&](const Word* w, bool& present) -> double {
    present = w != nullptr;
    if (!w) return NAN;
    int64_t v;
    if (!parseFixed(text + w->start, w->end - w->start, q_, &v)) {
      flags |= kPrecisionOverflow;
      return NAN;
    }
    return static_cast<double>(v);
  };
  bool hx = false, hy = false;
  const double vx = fixedOf(wx, hx), vy = fixedOf(wy, hy);
  const bool hz = wz != nullptr;
  const double vz = hz ? parseDecimal(text + wz->start, wz->end - wz->start) : NAN;
  const bool incremental = distance_ == 1;

  const auto endFixed = [&](double start, bool present, double v) -> double {
    if (!present) return start;
    if (!incremental) return v;
    if (!known(start) || !known(v)) {
      flags |= kIncrementalFromUnknown;
      return NAN;
    }
    const double r = start + v;
    if (!isSafeInteger(r)) {
      flags |= kPrecisionOverflow;
      return NAN;
    }
    return r;
  };
  const auto endZ = [&]() -> double { return !hz ? z_ : incremental ? (known(z_) ? z_ + vz : NAN) : vz; };
  const bool inPrimary = primary_ < 0 || units_ == primary_;
  const auto toPrimary = [&](double v) { return inPrimary ? v : primary_ == 1 ? v * 25.4 : v / 25.4; };
  const auto addPoint = [&](double fx, double fy) {
    if (inPrimary) boxAdd(exact_, fx, fy);
    else boxAdd(display_, toPrimary(toNumber(fx, q_)), toPrimary(toNumber(fy, q_)));
  };
  const auto addFloatPoint = [&](double px, double py) {
    if (inPrimary) boxAdd(arcExact_, px, py);
    else boxAdd(display_, toPrimary(px), toPrimary(py));
  };

  st.arc.state = ArcState::None;
  st.arc.reasonLength = 0;

  if (nonModal == 921 || nonModal == 922 || nonModal == 923) {
    x_ = y_ = z_ = NAN;
  } else if (owner == Owner::G92) {
    if (hx) x_ = vx;
    if (hy) y_ = vy;
    if (hz) z_ = vz;
  } else if (owner == Owner::G10) {
    const Word* l = s.word('L');
    const Word* wp = s.word('P');
    const double lv = l ? parseDecimal(text + l->start, l->end - l->start) : NAN;
    const double pv = wp ? parseDecimal(text + wp->start, wp->end - wp->start) : NAN;
    if (lv == 20 && (pv == 0 || (wcs_ != 0 && pv == wcs_))) {
      if (hx) x_ = vx;
      if (hy) y_ = vy;
      if (hz) z_ = vz;
    } else if (!(lv == 20 && wcs_ != 0 && pv >= 1 && pv <= 6)) {
      if (wx) x_ = NAN;
      if (wy) y_ = NAN;
      if (wz) z_ = NAN;
    }
  } else if (owner == Owner::G53 || owner == Owner::G38 || ((nonModal == 280 || nonModal == 300) && hasAxis)) {
    if (wx) x_ = NAN;
    if (wy) y_ = NAN;
    if (wz) z_ = NAN;
  } else if (nonModal == 280 || nonModal == 300) {
    x_ = y_ = z_ = NAN;
  } else if (motionLine && motion_ != M_G80 && !isProbe(motion_)) {
    const double ex = endFixed(startX, hx, vx), ey = endFixed(startY, hy, vy);
    const bool startKnown = known(startX) && known(startY);
    const bool endKnown = known(ex) && known(ey);
    const bool changesXY = (wx || wy) || motion_ == M_G2 || motion_ == M_G3;

    if (isCanned(motion_)) {
      x_ = ex;
      y_ = ey;
      z_ = NAN;
    } else if (motion_ == M_G0 || motion_ == M_G1) {
      if (startKnown && endKnown) {
        addPoint(startX, startY);
        addPoint(ex, ey);
      } else if (changesXY && !startKnown) {
        if (endKnown) addPoint(ex, ey);
        else if (everKnownXY_ || incremental) flags |= kUnknownStart;
      }
      x_ = ex;
      y_ = ey;
      z_ = endZ();
    } else {
      const bool turns = (motion_ == M_G2 ? s.hasG2 : s.hasG3) && !s.hasG4 && (s.mMask & pOwnerM()) == 0 && s.has('P');
      if (plane_ != 0) {
        flags |= kArcNotXY;
        if (startKnown && endKnown) {
          boxAdd(display_, toPrimary(toNumber(startX, q_)), toPrimary(toNumber(startY, q_)));
          boxAdd(display_, toPrimary(toNumber(ex, q_)), toPrimary(toNumber(ey, q_)));
        }
      } else if (!startKnown || !endKnown) {
        flags |= kUnknownStart;
      } else {
        const Word* wr = s.word('R');
        ArcInfo& info = st.arc;
        if (turns) {
          info.state = ArcState::Invalid;
          const char* r = "P (number of turns) isn't supported on arcs";
          info.reasonLength = static_cast<uint8_t>(std::strlen(r));
          std::memcpy(info.reason, r, info.reasonLength);
        } else if (wr && (s.has('I') || s.has('J') || s.has('K'))) {
          info.state = ArcState::Invalid;
          const char* r = "R together with I/J/K";
          info.reasonLength = static_cast<uint8_t>(std::strlen(r));
          std::memcpy(info.reason, r, info.reasonLength);
        } else if (!wr && arcDistance_ == 0) {
          info.state = ArcState::Invalid;
          const char* r = "absolute arc centres (G90.1) aren't supported";
          info.reasonLength = static_cast<uint8_t>(std::strlen(r));
          std::memcpy(info.reason, r, info.reasonLength);
        } else {
          bool hi = false, hj = false, hr = false;
          const double fi = fixedOf(s.word('I'), hi), fj = fixedOf(s.word('J'), hj), fr = fixedOf(wr, hr);
          ArcInput a;
          a.clockwise = motion_ == M_G2;
          a.sx = toNumber(startX, q_);
          a.sy = toNumber(startY, q_);
          a.ex = toNumber(ex, q_);
          a.ey = toNumber(ey, q_);
          a.rForm = wr != nullptr;
          a.r = toNumber(fr, q_);
          a.hasI = hi;
          a.hasJ = hj;
          a.i = hi ? toNumber(fi, q_) : 0;
          a.j = hj ? toNumber(fj, q_) : 0;
          a.inches = units_ == 0;
          interpretArc(a, info);
        }
        if (info.state == ArcState::Ok) {
          addPoint(startX, startY);
          addPoint(ex, ey);
          for (int k = 0; k < info.geometry.extremeCount; k++) addFloatPoint(info.geometry.extremeX[k], info.geometry.extremeY[k]);
        } else {
          flags |= kArcInvalid;
        }
      }
      x_ = ex;
      y_ = ey;
      z_ = endZ();
    }
    if (endKnown && !isCanned(motion_)) everKnownXY_ = true;
  }

  st.motion = static_cast<Motion>(motion_);
  st.plane = static_cast<Plane>(plane_);
  st.distance = static_cast<Distance>(distance_);
  st.arcDistance = static_cast<ArcDistance>(arcDistance_);
  st.units = static_cast<UnitsMode>(units_);
  st.wcs = static_cast<Wcs>(wcs_);
  st.feedMode = static_cast<FeedMode>(feed_);
  st.owner = owner;
  st.precision = static_cast<uint8_t>(q_);
  st.isXYMotion = isXYMove;
  st.flags = flags;
  st.letters = s.allLetters;
  st.gLo = s.gLo;
  st.gHi = s.gHi;
  st.mBits = s.mMask;
  // `before`: the previous line's position, rescaled by this line's rise as TS rescales it.
  st.before.x = prevX;
  st.before.y = prevY;
  st.before.z = prevZ;
  if (raiseCount_ > 0 && raises_[raiseCount_ - 1].line == i) {
    for (int k = 0; k < raiseCount_; k++) {
      if (raises_[k].line != i) continue;
      if (known(st.before.x)) st.before.x *= raises_[k].factor;
      if (known(st.before.y)) st.before.y *= raises_[k].factor;
    }
  }
  st.after.x = x_;
  st.after.y = y_;
  st.after.z = z_;
}

void Simulator::finish(Extents& extents, Extents& display, Units& units, bool& assumed, bool& mixed) const {
  double disp[4];
  std::memcpy(disp, display_, sizeof disp);
  extents.present = false;
  if (!boxEmpty(exact_) || !boxEmpty(arcExact_)) {
    const auto lo = [&](double a, double b) { return jsMin(a, b == INF ? INF : toFixed(b, q_)); };
    const auto hi = [&](double a, double b) { return jsMax(a, b == -INF ? -INF : toFixed(b, q_)); };
    extents.present = true;
    extents.minX = lo(exact_[0], arcExact_[0]);
    extents.maxX = hi(exact_[1], arcExact_[1]);
    extents.minY = lo(exact_[2], arcExact_[2]);
    extents.maxY = hi(exact_[3], arcExact_[3]);
    boxAdd(disp, toNumber(extents.minX, q_), toNumber(extents.minY, q_));
    boxAdd(disp, toNumber(extents.maxX, q_), toNumber(extents.maxY, q_));
  }
  display.present = !boxEmpty(disp);
  display.minX = disp[0];
  display.maxX = disp[1];
  display.minY = disp[2];
  display.maxY = disp[3];
  const int u = primary_ >= 0 ? primary_ : firstExplicitUnits_ >= 0 ? firstExplicitUnits_ : 1;
  units = u == 1 ? Units::Mm : Units::In;
  assumed = primary_ >= 0 ? assumedUnits_ : firstExplicitUnits_ < 0;
  mixed = mixed_;
}

}  // namespace gcode
