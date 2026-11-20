// Rewriting (ops/rotate90.ts): transformFor, setupTransform, the slots of a line, computeReps and
// writeReps, transformBox and rewriteStock. All the arithmetic is on fixed-point values held in
// doubles, as TS has it.
#include "rewrite.h"

#include <cmath>
#include <cstring>

#include "gcode/num.h"
#include "gcode/tokens.h"
#include "stock.h"

namespace gcode {
namespace {

/** JS Math.min / Math.max (NaN wins; -0 below +0). */
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

// ---- The transform (transformFor) ----

double xFromY(const TransformSetup& t, double y) { return t.mirror ? y : t.clockwise ? y + t.cx : t.cx - y; }
double yFromX(const TransformSetup& t, double x) { return t.mirror ? x : t.clockwise ? t.cy - x : x + t.cy; }
double dxFromDy(const TransformSetup& t, double dy) { return t.mirror || t.clockwise ? dy : -dy; }
double dyFromDx(const TransformSetup& t, double dx) { return t.mirror || !t.clockwise ? dx : -dx; }
double iFromJ(const TransformSetup& t, double j) { return t.mirror || t.clockwise ? j : -j; }
double jFromI(const TransformSetup& t, double i) { return t.mirror || !t.clockwise ? i : -i; }

void transformFor(TransformKind kind, double minX, double maxX, double minY, double maxY, TransformSetup& t) {
  t.mirror = kind == TransformKind::FlipH || kind == TransformKind::FlipV;
  t.mirrorX = kind == TransformKind::FlipH;
  t.sum = 0;
  t.cx = t.cy = 0;
  t.clockwise = kind == TransformKind::RotateCW;
  if (t.mirror) {
    t.sum = t.mirrorX ? minX + maxX : minY + maxY;
  } else if (t.clockwise) {
    // (x, y) → (y, −x), then X' = Y + (minX − minY), Y' = −X + (maxX + minY)
    t.cx = minX - minY;
    t.cy = maxX + minY;
  } else {
    // (x, y) → (−y, x), then X' = −Y + (maxY + minX), Y' = X + (minY − minX)
    t.cx = maxY + minX;
    t.cy = minY - minX;
  }
}

// ---- The slots of a line ----

/** The first X, Y, I and J word on the line, and its G2/G3 word (a mirror swaps it). */
struct Slot {
  bool present;
  uint32_t start, end, valueStart;
  bool lower;        // the letter was lower case
  bool leadingZero;  // the number's style (styleOf): `.25` has none
};

/** Whether the number in text[start, end) is 2 or 3 (`2`, `02`, `3.0`): an arc's G-code. */
bool isArcCode(const char* text, uint32_t start, uint32_t end) {
  uint32_t i = start;
  while (i + 1 < end && text[i] == '0') i++;
  if (i >= end || (text[i] != '2' && text[i] != '3')) return false;
  i++;
  if (i == end) return true;
  if (text[i] != '.') return false;
  for (i++; i < end; i++) {
    if (text[i] != '0') return false;
  }
  return true;
}

class SlotSink final : public TokenSink {
 public:
  explicit SlotSink(const char* text) : text_(text) {}
  Slot x{}, y{}, i{}, j{}, g{};
  void word(uint32_t start, uint32_t end, char letter, uint32_t valueStart, int) override {
    Slot* s = letter == 'X' ? &x : letter == 'Y' ? &y : letter == 'I' ? &i : letter == 'J' ? &j
              : letter == 'G' && isArcCode(text_, valueStart, end) ? &g : nullptr;
    if (!s || s->present) return;
    s->present = true;
    s->start = start;
    s->end = end;
    s->valueStart = valueStart;
    s->lower = static_cast<unsigned char>(text_[start]) >= 97;
    const char c = text_[valueStart];
    s->leadingZero = (c == '+' || c == '-' ? text_[valueStart + 1] : c) != '.';
  }
  void space(uint32_t, uint32_t) override {}
  void comment(uint32_t, uint32_t, CommentStyle) override {}
  void raw(uint32_t, uint32_t) override {}

 private:
  const char* text_;
};

/** slotValue: the slot's value at precision p, or NaN when it isn't a safe integer there. */
double slotValue(const char* text, const Slot& s, int p) {
  int64_t v;
  return parseFixed(text + s.valueStart, s.end - s.valueStart, p, &v) ? static_cast<double>(v) : NAN;
}

struct Rep {
  const Slot* slot;
  char letter;
  double value;
  bool swap;  // the G2/G3 word: its significant digit swaps
};

struct Reps {
  Rep r[5];
  int n = 0;
  void add(const Slot* slot, char letter, double value, bool swap = false) {
    // In slot order (insertion sort over at most four).
    int k = n++;
    while (k > 0 && r[k - 1].slot->start > slot->start) {
      r[k] = r[k - 1];
      k--;
    }
    r[k] = Rep{slot, letter, value, swap};
  }
};

void pairSlots(const char* text, int p, const Slot& a, const Slot& b, char la, char lb, double (*aFromB)(const TransformSetup&, double),
               double (*bFromA)(const TransformSetup&, double), const TransformSetup& t, Reps& reps) {
  if (a.present && b.present) {
    const double va = slotValue(text, a, p), vb = slotValue(text, b, p);
    const bool aFirst = a.start < b.start;
    reps.add(aFirst ? &a : &b, la, aFromB(t, vb));
    reps.add(aFirst ? &b : &a, lb, bFromA(t, va));
  } else if (a.present) {
    reps.add(&a, lb, bFromA(t, slotValue(text, a, p)));
  } else if (b.present) {
    reps.add(&b, la, aFromB(t, slotValue(text, b, p)));
  }
}

}  // namespace

bool setupTransform(TransformKind kind, const Analysis& a, bool hasG10, TransformSetup& out) {
  const Extents& e = a.extents;
  if (!e.present) return false;
  // The box whose min corner stays put: the stock, when the file declares one; otherwise the
  // toolpath, without the fixed points of the setup when there are any (06 §1, 09 D45).
  const Extents& jobBox = (a.fixedMoveCount || hasG10) && a.jobExtents.present ? a.jobExtents : e;
  const StockComment* pivotStock = a.stockCommentCount ? &a.stockComments[a.stockCommentCount - 1] : nullptr;
  int q = a.precision;
  if (pivotStock) {
    int digits = 0;
    for (int k = 0; k < 4; k++) {
      if (pivotStock->values[k].fractionDigits > digits) digits = pivotStock->values[k].fractionDigits;
    }
    if (digits > q) q = digits;
  }
  const double scale = pow10(q - a.precision);
  Extents pivot{true, jobBox.minX * scale, jobBox.maxX * scale, jobBox.minY * scale, jobBox.maxY * scale};
  Extents stock{};
  if (pivotStock && stockBox(*pivotStock, q, stock)) pivot = stock;
  transformFor(kind, pivot.minX, pivot.maxX, pivot.minY, pivot.maxY, out);
  out.q = q;
  return true;
}

bool wordEdits(const char* text, size_t length, const TransformSetup& t, bool incremental, Edit* out, int& count) {
  SlotSink s(text);
  scanLine(text, length, s);
  Reps reps;
  const int p = t.q;
  if (t.mirror) {
    const Slot& axis = t.mirrorX ? s.x : s.y;
    const Slot& offset = t.mirrorX ? s.i : s.j;
    if (axis.present) {
      const double v = slotValue(text, axis, p);
      reps.add(&axis, t.mirrorX ? 'X' : 'Y', incremental ? -v : t.sum - v);
    }
    if (offset.present) reps.add(&offset, t.mirrorX ? 'I' : 'J', -slotValue(text, offset, p));
    if (s.g.present) reps.add(&s.g, 'G', 0, true);
  } else {
    pairSlots(text, p, s.x, s.y, 'X', 'Y', incremental ? dxFromDy : xFromY, incremental ? dyFromDx : yFromX, t, reps);
    pairSlots(text, p, s.i, s.j, 'I', 'J', iFromJ, jFromI, t, reps);
  }
  count = 0;
  for (int k = 0; k < reps.n; k++) {
    const Rep& r = reps.r[k];
    Edit& e = out[count++];
    if (r.swap) {
      // The code's significant digit swaps, keeping the word's own spelling (`G02` → `G03`).
      uint32_t d = r.slot->valueStart;
      while (d + 1 < r.slot->end && text[d] == '0' && text[d + 1] != '.') d++;
      e.start = d;
      e.end = d + 1;
      e.text[0] = text[d] == '2' ? '3' : '2';
      e.length = 1;
      continue;
    }
    if (!isSafeInteger(r.value)) return false;
    e.start = r.slot->start;
    e.end = r.slot->end;
    e.text[0] = r.slot->lower ? static_cast<char>(r.letter + 32) : r.letter;
    const int n = formatFixed(r.value, p, r.slot->leadingZero, e.text + 1, sizeof(e.text) - 1);
    if (n < 0) return false;
    e.length = static_cast<uint8_t>(n + 1);
  }
  return true;
}

namespace {

class StockRewriter final : public StockVisitor {
 public:
  StockRewriter(const TransformSetup& t, EditSink* sink) : t_(t), sink_(sink) {}
  bool comment(const StockComment& c) override {
    Extents box{};
    if (!stockBox(c, t_.q, box)) return false;
    // transformBox: new X comes from old Y, new Y from old X.
    double next[4];
    if (t_.mirror && t_.mirrorX) {
      next[0] = t_.sum - box.maxX;
      next[1] = t_.sum - box.minX;
      next[2] = box.minY;
      next[3] = box.maxY;
    } else if (t_.mirror) {
      next[0] = box.minX;
      next[1] = box.maxX;
      next[2] = t_.sum - box.maxY;
      next[3] = t_.sum - box.minY;
    } else {
      const double x1 = xFromY(t_, box.minY), x2 = xFromY(t_, box.maxY), y1 = yFromX(t_, box.minX), y2 = yFromX(t_, box.maxX);
      next[0] = jsMin(x1, x2);
      next[1] = jsMax(x1, x2);
      next[2] = jsMin(y1, y2);
      next[3] = jsMax(y1, y2);
    }
    for (double v : next) {
      if (!isSafeInteger(v)) return false;
    }
    if (!sink_) return true;
    const double old[4] = {box.minX, box.maxX, box.minY, box.maxY};
    // The XY bounds that change (a rotation rewrites all four), in the order they stand.
    int keys[4];
    int n = 0;
    for (int k = 0; k < 4; k++) {
      if (t_.mirror && next[k] == old[k]) continue;
      int at = n++;
      while (at > 0 && c.values[keys[at - 1]].start > c.values[k].start) {
        keys[at] = keys[at - 1];
        at--;
      }
      keys[at] = k;
    }
    for (int m = 0; m < n; m++) {
      const StockValue& v = c.values[keys[m]];
      Edit e{};
      e.start = v.start;
      e.end = v.end;
      const int len = formatFixed(next[keys[m]], t_.q, true, e.text, sizeof e.text);
      if (len < 0) return false;
      e.length = static_cast<uint8_t>(len);
      if (!sink_->edit(e)) return false;
    }
    return true;
  }

 private:
  const TransformSetup& t_;
  EditSink* sink_;
};

}  // namespace

bool stockEdits(const char* text, size_t length, uint32_t line, const TransformSetup& t, EditSink* sink) {
  StockRewriter r(t, sink);
  return eachStockComment(text, length, line, r);
}

}  // namespace gcode
