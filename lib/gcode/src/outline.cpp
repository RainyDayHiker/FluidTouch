// The Outline Check (specs/15 §5.4, §18, C3): ops/outline-check.ts, step for step.
//
//   Setup (after pass 1):  the report's fixed part at the arena's top, and below it the grid
//                          (2 bits a cell) over the job's extents.
//   Each line (pass 2):    the rules of 13 §4, the path drawn into the grid, the preamble's
//                          G10 L20 words and the first G0's F copied.
//   Finish (after pass 2): fill the outside, trace each shape, simplify, order the loops; then the
//                          grid and the working memory are given back, and only the report, the
//                          words, the corners and the loop starts stay at the top (15 §18.3).
//   Write (on request):    the file of 13 §3 through a buffer from the arena; no read.
//
// The outline never changes the analysis: when the analysis needs memory it holds, or it runs
// out, or Finish is cancelled, it stops, gives its memory back and says why (15 §18.2).
#include "gcode/outline.h"

#include <cmath>
#include <cstring>

#include "analyse_internal.h"
#include "arena.h"
#include "gcode/num.h"
#include "gcode/tokens.h"
#include "outline_internal.h"
#include "vocabulary.h"

namespace gcode {
namespace {

// ---- Constants (outline-check.ts) ----

/** The grid never has more cells than this; a larger job gets larger cells. */
const double MAX_CELLS = 1000000;
/** No arc is split into more pieces than this, whatever its radius. */
const double MAX_ARC_SEGMENTS = 4096;
/** One issue per rule, ten rules (13 §4). */
const uint32_t kMaxIssues = 10;

/** The grid's cells: the TS values, two bits each. */
const uint8_t EMPTY = 0, PATH = 1, OUTSIDE = 2, TRACED = 3;

/** The eight neighbours, counter-clockwise from east (y up). */
const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
/** Direction index of the neighbour at (dx, dy): DIR[(dx + 1) + (dy + 1) * 3]. */
const int DIR[9] = {5, 6, 7, 4, -1, 0, 3, 2, 1};

constexpr uint32_t letter(char c) { return 1u << (c - 'A'); }
const uint32_t EXTRA_AXES = letter('A') | letter('B') | letter('C') | letter('U') | letter('V') | letter('W');
const uint32_t NOT_UNDERSTOOD = kMalformedNumber | kUnclosedComment | kParametricSyntax | kChecksum | kUnrecognizedSyntax | kUnknownLetter |
                                kUnknownGCode | kDuplicateWord | kModalGroupConflict;

const char* const NO_CUTTING_MOVES = "No cutting moves found";

// The G-codes the rules look for, as code keys (integer part × 10 + one fraction digit).
const int G10[] = {100};
const int G92[] = {920, 921, 922, 923};
const int CANNED[] = {730, 760, 810, 820, 830, 840, 850, 860, 870, 880, 890};
const int COMP[] = {410, 420};

/** A set of G-codes as the bits of LineState::gLo / gHi, so a line's test is two ANDs. */
struct GSet {
  uint32_t lo = 0, hi = 0;
  template <size_t N>
  explicit GSet(const int (&keys)[N]) {
    for (int key : keys) {
      const int b = vocab::gIndex(key);
      if (b >= 0 && b < 32) lo |= 1u << b;
      else if (b >= 32) hi |= 1u << (b - 32);
    }
  }
  bool any(const LineState& st) const { return (st.gLo & lo) != 0 || (st.gHi & hi) != 0; }
};

bool known(double v) { return !std::isnan(v); }

/** JS Math.ceil, from floor (15 §4.4: <cmath> gives floor). */
double jsCeil(double x) { return -std::floor(-x); }

size_t align8(size_t n) { return (n + 7) / 8 * 8; }

const char* endingText(Ending e, size_t& n) {
  switch (e) {
    case Ending::CRLF: n = 2; return "\r\n";
    case Ending::CR: n = 1; return "\r";
    default: n = 1; return "\n";
  }
}

/**
 * The first word of each letter on a line, from its own scan: the outline needs words only on
 * G10 lines and the first G0 with an F, so analyse() collects no tokens for it.
 */
class Words final : public TokenSink {
 public:
  /** Scans the line only when `scan` is set; without, it mustn't be asked (most lines). */
  Words(const char* text, size_t n, bool scan) : text_(text) {
    if (!scan) return;
    std::memset(has_, 0, sizeof has_);
    scanLine(text, n, *this);
  }
  void word(uint32_t start, uint32_t end, char letter, uint32_t valueStart, int) override {
    const int k = letter - 'A';
    if (k < 0 || k >= 26 || has_[k]) return;
    has_[k] = true;
    start_[k] = start;
    end_[k] = end;
    value_[k] = valueStart;
  }
  void space(uint32_t, uint32_t) override {}
  void comment(uint32_t, uint32_t, CommentStyle) override {}
  void raw(uint32_t, uint32_t) override {}

  bool has(char l) const { return has_[l - 'A']; }
  /** The word's text, letter included. */
  Str text(char l) const { return Str{text_ + start_[l - 'A'], end_[l - 'A'] - start_[l - 'A']}; }
  /** The word's number text (valueText). */
  Str value(char l) const { return Str{text_ + value_[l - 'A'], end_[l - 'A'] - value_[l - 'A']}; }

 private:
  const char* text_;
  bool has_[26];
  uint32_t start_[26], end_[26], value_[26];
};

/** /^L[ \t]*0*20(\.0*)?$/i on the line's first L word (validateOutlineCheck's L20). */
bool firstLIsL20(const Words& words) {
  if (!words.has('L')) return false;
  const Str t = words.text('L');
  const char* p = t.data;
  const uint32_t n = t.length;
  uint32_t j = 1;
  while (j < n && (p[j] == ' ' || p[j] == '\t')) j++;
  while (j < n && p[j] == '0') j++;
  bool ok = j + 2 <= n && p[j] == '2' && p[j + 1] == '0';
  j += 2;
  if (ok && j < n) {
    ok = p[j] == '.';
    for (j++; ok && j < n; j++) ok = p[j] == '0';
  }
  return ok;
}

// ---- What the outline keeps (at the arena's top) ----

struct Held {
  Status status;  // Ok, or why the outline isn't there
  // The report (13 §4), in makeReport's order.
  Issue issues[kMaxIssues];
  uint32_t issueCount;
  bool blocked, warnings;
  // For the file and the summary.
  bool empty;
  uint8_t precision;
  Units units;
  bool unitsAssumed;
  Wcs wcs;        // at the first XY motion
  Ending ending;  // the first line ending; LF without one
  const char* zero;  // the preamble's G10 L20 lines, each ended by '\n'
  uint32_t zeroLength;
  const char* feed;  // the first G0's F text, when it has one in G94
  uint32_t feedLength;
  bool hasFeed;
  double startX, startY;
  const double* corners;  // x, y pairs, in the order written
  const uint32_t* loopStarts;
  uint32_t loopCount, cornerCount;
  double tolerance;
  char toleranceText[32];
  uint32_t toleranceLength;
  char summary[192];
  uint32_t summaryLength;
};

/** A text built in a fixed buffer. */
struct Text {
  char* buf;
  size_t cap;
  size_t len = 0;
  void put(const char* s, size_t n) {
    for (size_t k = 0; k < n && len < cap; k++) buf[len++] = s[k];
  }
  void put(const char* s) { put(s, std::strlen(s)); }
  void count(double n) {
    char num[32];
    const int k = formatCount(n, num, sizeof num);
    if (k > 0) put(num, static_cast<size_t>(k));
  }
  void fixed(double v, int p) {
    char num[48];
    const int k = formatFixed(v, p, true, num, sizeof num);
    if (k > 0) put(num, static_cast<size_t>(k));
  }
};

// ---- The grid (13 §2) ----

struct Grid {
  uint8_t* g = nullptr;
  int w = 0, h = 0;
  uint32_t n = 0;  // cells
  double size = 0, minX = 0, minY = 0;

  uint8_t get(uint32_t i) const { return static_cast<uint8_t>((g[i >> 2] >> ((i & 3u) * 2)) & 3u); }
  void set(uint32_t i, uint8_t v) {
    uint8_t& b = g[i >> 2];
    const unsigned s = (i & 3u) * 2;
    b = static_cast<uint8_t>((b & ~(3u << s)) | (static_cast<unsigned>(v) << s));
  }

  // The cell whose centre is nearest, inside the margin. (TS never meets a NaN here; it would
  // loop forever on one, so taking the margin's first cell isn't a difference that can show.)
  int gx(double x) const {
    const double c = std::floor((x - minX) / size + 0.5) + 1;
    if (!(c >= 1)) return 1;
    return c > w - 2 ? w - 2 : static_cast<int>(c);
  }
  int gy(double y) const {
    const double c = std::floor((y - minY) / size + 0.5) + 1;
    if (!(c >= 1)) return 1;
    return c > h - 2 ? h - 2 : static_cast<int>(c);
  }

  /** Marks the cells of a line (8-connected, so the outside can't leak through it). */
  void line(double x0, double y0, double x1, double y1) {
    int cx = gx(x0), cy = gy(y0);
    const int ex = gx(x1), ey = gy(y1);
    const int dx = ex > cx ? ex - cx : cx - ex, dy = -(ey > cy ? ey - cy : cy - ey);
    const int sx = cx < ex ? 1 : -1, sy = cy < ey ? 1 : -1;
    int err = dx + dy;
    for (;;) {
      set(static_cast<uint32_t>(cx + cy * w), PATH);
      if (cx == ex && cy == ey) return;
      const int e2 = 2 * err;
      if (e2 >= dy) {
        err += dy;
        cx += sx;
      }
      if (e2 <= dx) {
        err += dx;
        cy += sy;
      }
    }
  }
};

// ---- The builder: a FinalPass inside analyse() ----

class Builder final : public FinalPass, public ArenaYield {
 public:
  Builder(ArenaAlloc& arena, const AnalysisOptions& options, bool reportOnly, OutlineTuning& tuning)
      : arena_(arena), options_(options), reportOnly_(reportOnly), tuning_(tuning), g10_(G10), g92_(G92), canned_(CANNED), comp_(COMP) {}

  // ---- Setup ----

  bool begin(const Analysis& a, const PassOneFacts&) override {
    phase_ = 1;
    baseTop_ = arena_.top();
    held_ = static_cast<Held*>(arena_.allocTop(sizeof(Held), alignof(Held)));
    if (!held_) {
      status_ = Status::OutOfMemory;
      tuning_.stoppedIn = 1;
      return true;
    }
    std::memset(static_cast<void*>(held_), 0, sizeof(Held));
    held_->status = Status::Ok;
    held_->precision = a.precision;
    held_->units = a.units;
    held_->unitsAssumed = a.unitsAssumed;
    held_->wcs = Wcs::Initial;
    held_->ending = Ending::LF;
    held_->zero = "";
    held_->feed = "";
    fixedTop_ = arena_.top();
    first_ = a.firstXYMotion;
    lineCount_ = a.lineCount;
    P_ = a.precision;
    scale_ = pow10(P_);
    arena_.setYield(this);
    if (reportOnly_ || !a.jobExtents.present) return true;

    // The stock's top: the last valid stock comment's ZMAX (buildOutlineCheck).
    if (a.stockCommentCount) {
      const Str z = a.stockComments[a.stockCommentCount - 1].values[5].text;
      hasStockTop_ = true;
      stockTop_ = parseDecimal(z.data, z.length);
    }
    zTolerance_ = 1 / pow10(P_);  // CAM rounds Z and the stock separately (12 §4)

    // Grid: square cells from the job's lowest corner, one cell of margin all round.
    const Extents& box = a.jobExtents;
    const double finest = (a.units == Units::In ? 0.02 : 0.5) * scale_;
    const double fit = std::sqrt(((box.maxX - box.minX + 1) * (box.maxY - box.minY + 1)) / MAX_CELLS);
    double size = fit > finest ? fit : finest;
    double w, h;
    for (;;) {
      w = std::floor((box.maxX - box.minX) / size + 0.5) + 3;
      h = std::floor((box.maxY - box.minY) / size + 0.5) + 3;
      if (w * h <= MAX_CELLS) break;
      size *= 1.02;
    }
    grid_.size = size;
    grid_.minX = box.minX;
    grid_.minY = box.minY;
    grid_.w = static_cast<int>(w);
    grid_.h = static_cast<int>(h);
    grid_.n = static_cast<uint32_t>(grid_.w) * static_cast<uint32_t>(grid_.h);
    const size_t bytes = (grid_.n + 3) / 4;
    grid_.g = static_cast<uint8_t*>(arena_.allocTop(bytes, 8));
    if (!grid_.g) {
      drop(Status::OutOfMemory);
      return true;
    }
    std::memset(grid_.g, 0, bytes);
    gridTop_ = arena_.top();
    sampleTolerance_ = grid_.size / 2 / scale_;
    phase_ = 2;
    return true;
  }

  // ---- Each line of pass 2 ----

  bool wantsTokens() const override { return false; }

  bool line(const Line& line, const Token*, uint16_t, const LineState& st) override {
    if (!held_) return true;
    phase_ = 2;
    const uint32_t i = line.index;
    size_t endLength = 0;
    if (line.ending != Ending::None) endingText(line.ending, endLength);
    fileBytes_ = line.offset + line.length + endLength;
    if (!haveEnding_ && line.ending != Ending::None) {
      held_->ending = line.ending;
      haveEnding_ = true;
    }
    if (static_cast<int32_t>(i) == first_) held_->wcs = st.wcs;
    at_ = At{i + 1, line.offset, line.length};
    const bool g10 = g10_.any(st);
    const bool feedLine = !reportOnly_ && !feedDone_ && (st.letters & letter('F')) && st.motion == Motion::G0;
    const Words words(line.data, line.length, g10 || feedLine);  // only the lines that need them
    rules(words, st, i);
    fileLevel(st);
    if (reportOnly_) return true;

    const bool beforeMotion = first_ < 0 || static_cast<int32_t>(i) < first_;
    if (feedLine) rapidFeed(words, st);
    if (held_ && beforeMotion && g10) zeroLine(words, st);
    if (held_ && grid_.g) draw(st);
    return true;
  }

  // ---- The arena wants the top back ----

  void yieldTop() override { drop(Status::OutOfMemory); }

  // ---- After pass 2 ----

  /** Finish, then the result's outline fields. */
  void finish(Analysis& result, Status analysisStatus) {
    arena_.setYield(nullptr);
    phase_ = 3;
    if (analysisStatus != Status::Ok) drop(analysisStatus);
    if (held_) {
      sortReport();
      if (!reportOnly_) build();
    }
    result.outlineStatus = held_ ? Status::Ok : status_;
    result.outline = held_;
    result.outlineHeld = held_ ? arena_.top() - baseTop_ : 0;
    result.arenaPeak = arena_.peak();
  }

 private:
  struct At {
    uint32_t line;
    uint64_t offset;
    uint32_t length;
  };

  /** Stops: gives the top back and says why. */
  void drop(Status s) {
    if (!held_ && status_ != Status::Ok) return;
    if (held_ && tuning_.stoppedIn == 0) tuning_.stoppedIn = phase_;
    status_ = s;
    held_ = nullptr;
    grid_.g = nullptr;
    arena_.setTop(baseTop_);
    arena_.setYield(nullptr);
  }

  bool tick() { return !options_.progress || options_.progress(options_.progressContext, lineCount_, fileBytes_); }

  // ---- Validation (validateOutlineCheck, 13 §4) ----

  void report(uint8_t source, const char* ruleId, Severity severity, const char* reason) {
    for (uint32_t k = 0; k < held_->issueCount; k++) {
      if (std::strcmp(held_->issues[k].ruleId, ruleId) == 0) return;  // each rule once, at its first line
    }
    if (held_->issueCount >= kMaxIssues) return;
    const uint32_t k = held_->issueCount++;
    Issue& is = held_->issues[k];
    is.line = at_.line;
    is.offset = at_.offset;
    is.length = at_.length;
    is.severity = severity;
    is.ruleId = ruleId;
    is.reason = Str{reason, static_cast<uint32_t>(std::strlen(reason))};
    is.hasChoices = false;
    source_[k] = source;
    if (severity == Severity::Blocking) held_->blocked = true;
    if (severity == Severity::Warning) held_->warnings = true;
  }

  void rules(const Words& words, const LineState& st, uint32_t i) {
    const uint32_t flags = st.parseFlags | st.flags;
    if (g10_.any(st)) {
      const bool beforeMotion = first_ < 0 || static_cast<int32_t>(i) < first_;
      if (!(beforeMotion && firstLIsL20(words))) {
        report(0, "B-G10", Severity::Blocking, beforeMotion ? "Work offset change (G10) other than L20" : "Work offset change (G10) mid-file");
      }
    }
    if (g92_.any(st)) {
      report(0, "B-G92", Severity::Blocking, "Coordinate offset (G92) changes the coordinate system mid-file");
    }
    if (flags & (kArcInvalid | kArcNotXY)) {
      report(0, "W-ARC-LINE", Severity::Warning, "Arc drawn as a straight line: it can't be interpreted, or isn't in the XY plane");
    }
    if (flags & (kUnknownStart | kIncrementalFromUnknown)) {
      report(0, "W-UNKNOWN-POS", Severity::Warning, "Move from an unknown position: left out of the outline");
    }
    const bool canned = (st.owner == Owner::Motion && static_cast<int>(st.motion) >= static_cast<int>(Motion::G73)) || canned_.any(st);
    if (canned) report(0, "W-CANNED", Severity::Warning, "Canned cycle: its holes aren't in the outline");
    if (comp_.any(st)) report(0, "W-COMP", Severity::Warning, "Cutter compensation (G41/G42): the outline follows the programmed path");
    if (st.letters & EXTRA_AXES) report(0, "W-AXIS", Severity::Warning, "Rotary / extra axis: the outline is XY only");
    if (flags & NOT_UNDERSTOOD) report(0, "W-SYNTAX", Severity::Warning, "Line not understood: the outline may not match the cut");
  }

  /** rotate90.ts fileLevelIssues without W5 (G54's assumption doesn't move the outline). */
  void fileLevel(const LineState& st) {
    if (!st.isXYMotion || (unitsReported_ && wcsReported_)) return;
    if (!haveUnits_) {
      units_ = st.units;
      haveUnits_ = true;
    }
    if (!unitsReported_ && st.units != units_) {
      unitsReported_ = true;
      report(1, "B-UNITS", Severity::Blocking, "Mixed units: XY motion in both mm and inches");
    }
    const Wcs effective = st.wcs == Wcs::Initial ? Wcs::G54 : st.wcs;  // moves before any G54–G59 count as G54
    if (!haveFirstWcs_) {
      firstWcs_ = effective;
      haveFirstWcs_ = true;
    }
    if (!wcsReported_ && effective != firstWcs_) {
      wcsReported_ = true;
      report(1, "B-WCS", Severity::Blocking, "Multiple work coordinate systems in one file");
    }
  }

  /** makeReport's order: severity, line, then on one line the order the checks found them in. */
  void sortReport() {
    Issue* is = held_->issues;
    const uint32_t n = held_->issueCount;
    uint8_t seq[kMaxIssues];
    for (uint32_t k = 0; k < n; k++) seq[k] = static_cast<uint8_t>(k);
    const auto before = [&](uint32_t a, uint32_t b) {
      if (is[a].severity != is[b].severity) return is[a].severity < is[b].severity;
      if (is[a].line != is[b].line) return is[a].line < is[b].line;
      if (source_[seq[a]] != source_[seq[b]]) return source_[seq[a]] < source_[seq[b]];
      return seq[a] < seq[b];
    };
    for (uint32_t k = 1; k < n; k++) {
      for (uint32_t j = k; j > 0 && before(j, j - 1); j--) {
        const Issue t = is[j];
        is[j] = is[j - 1];
        is[j - 1] = t;
        const uint8_t s = seq[j];
        seq[j] = seq[j - 1];
        seq[j - 1] = s;
      }
    }
  }

  // ---- The words the file repeats (13 §3) ----

  /** rapidFeed: the first G0 line with an F; its F text only in G94. */
  void rapidFeed(const Words& words, const LineState& st) {
    feedDone_ = true;
    if (st.feedMode != FeedMode::G94 || !words.has('F')) return;
    const Str v = words.value('F');
    char* p = static_cast<char*>(arena_.allocTop(v.length ? v.length : 1, 1));
    if (!p) return drop(Status::OutOfMemory);
    std::memcpy(p, v.data, v.length);
    held_->feed = p;
    held_->feedLength = v.length;
    held_->hasFeed = true;
  }

  /**
   * zeroLines: a preamble G10 L20 with X or Y, written with its P, X and Y words. A G10 there that
   * isn't L20 blocks (B-G10), so when the file is written every G10 here is one.
   */
  void zeroLine(const Words& w, const LineState& st) {
    if (!firstLIsL20(w)) return;
    const char order[3] = {'P', 'X', 'Y'};
    Str words[3] = {{"", 0}, {"", 0}, {"", 0}};
    bool has[3] = {false, false, false};
    for (int j = 0; j < 3; j++) {
      has[j] = w.has(order[j]);
      if (has[j]) words[j] = w.value(order[j]);
    }
    if (!has[1] && !has[2]) return;
    size_t need = 8;  // "G10 L20" and '\n'
    for (int j = 0; j < 3; j++) need += has[j] ? 2 + words[j].length : 0;
    if (zeroLength_ + need > zeroCap_) {
      // A larger block below (the old one is left until Finish packs the outline).
      size_t cap = zeroCap_ ? zeroCap_ * 2 : 128;
      while (cap < zeroLength_ + need) cap *= 2;
      char* grown = static_cast<char*>(arena_.allocTop(cap, 1));
      if (!grown) return drop(Status::OutOfMemory);
      if (zeroLength_) std::memcpy(grown, zeroBuf_, zeroLength_);
      zeroBuf_ = grown;
      zeroCap_ = cap;
    }
    Text t{zeroBuf_ + zeroLength_, need};
    t.put("G10 L20");
    for (int j = 0; j < 3; j++) {
      if (!has[j]) continue;
      const char head[2] = {' ', order[j]};
      t.put(head, 2);
      t.put(words[j].data, words[j].length);
    }
    t.put("\n");
    zeroLength_ += t.len;
    held_->zero = zeroBuf_;
    held_->zeroLength = static_cast<uint32_t>(zeroLength_);
    hasStart_ = known(st.after.x) && known(st.after.y);
    startX_ = st.after.x;
    startY_ = st.after.y;
  }

  // ---- The cutting path (drawPath, 13 §1) ----

  bool above(double z) const { return hasStockTop_ && known(z) && z >= stockTop_ - zTolerance_; }

  /** A piece starts at (x, y), unless the last piece ended there. */
  void start(double x, double y) {
    if (drawn_ && lastX_ == x && lastY_ == y) return;
    grid_.line(x, y, x, y);
    drawn_ = true;
    lastX_ = x;
    lastY_ = y;
  }

  void draw(const LineState& st) {
    const Motion m = st.motion;
    if ((m != Motion::G1 && m != Motion::G2 && m != Motion::G3) || st.owner != Owner::Motion) return;
    const Position& b = st.before;
    const Position& a = st.after;
    if (!known(b.x) || !known(b.y) || !known(a.x) || !known(a.y)) return;
    if (above(b.z) && above(a.z)) return;
    if (!st.isXYMotion) {
      if (known(b.z) && known(a.z) && a.z < b.z) start(a.x, a.y);
      return;
    }
    start(b.x, b.y);
    if (st.arc.state == ArcState::Ok) {
      // arcPoints: chords within half a cell of the arc.
      const ArcGeometry& g = st.arc.geometry;
      const double step = sampleTolerance_ < g.r ? 2 * fmath::acos(1 - sampleTolerance_ / g.r) : HUGE_VAL;
      const double pieces = jsCeil(std::fabs(g.sweep) / step);
      const double atLeastOne = pieces > 1 ? pieces : 1;
      const int segments = static_cast<int>(atLeastOne < MAX_ARC_SEGMENTS ? atLeastOne : MAX_ARC_SEGMENTS);
      for (int k = 1; k < segments; k++) {
        const double t = g.startAngle + (g.sweep * k) / segments;
        const double ax = (g.cx + g.r * fmath::cos(t)) * scale_;
        const double ay = (g.cy + g.r * fmath::sin(t)) * scale_;
        grid_.line(lastX_, lastY_, ax, ay);
        lastX_ = ax;
        lastY_ = ay;
      }
    }
    grid_.line(lastX_, lastY_, a.x, a.y);
    lastX_ = a.x;
    lastY_ = a.y;
  }

  // ---- Finish ----

  /** Fills the outside (4-connected, from the margin) by scanlines, from the seeds on `stack`. */
  void fillFrom(uint32_t* stack, uint32_t cap, uint32_t& size, bool& dropped) {
    Grid& g = grid_;
    const auto push = [&](uint32_t v) {
      if (size == cap) dropped = true;  // the fallback's sweep finds it again
      else stack[size++] = v;
    };
    while (size) {
      const uint32_t i = stack[--size];
      if (g.get(i) != EMPTY) continue;
      const int y = static_cast<int>(i / static_cast<uint32_t>(g.w));
      const uint32_t row = static_cast<uint32_t>(y * g.w);
      int l = static_cast<int>(i - row), r = l;
      while (l > 0 && g.get(row + static_cast<uint32_t>(l) - 1) == EMPTY) l--;
      while (r < g.w - 1 && g.get(row + static_cast<uint32_t>(r) + 1) == EMPTY) r++;
      for (int x = l; x <= r; x++) g.set(row + static_cast<uint32_t>(x), OUTSIDE);
      for (int ny = y - 1; ny <= y + 1; ny += 2) {
        if (ny < 0 || ny >= g.h) continue;
        const uint32_t nrow = static_cast<uint32_t>(ny * g.w);
        for (int x = l; x <= r; x++) {
          const uint32_t c = nrow + static_cast<uint32_t>(x);
          if (g.get(c) == EMPTY && (x == l || g.get(c - 1) != EMPTY)) push(c);
        }
      }
    }
  }

  /**
   * fillOutside with a fixed stack (15 §18.3): a seed that doesn't fit is dropped; then sweeps of
   * the grid seed each EMPTY cell with an OUTSIDE 4-neighbour until nothing was dropped. The
   * filled set is the 4-connected component of EMPTY cells around the margin, whatever the order.
   */
  bool fillOutside() {
    const size_t bottom = arena_.used();
    const uint32_t cap = tuning_.fillSeeds ? tuning_.fillSeeds : 1;
    uint32_t* stack = static_cast<uint32_t*>(arena_.tryAlloc(cap * sizeof(uint32_t), alignof(uint32_t)));
    if (!stack) {
      drop(Status::OutOfMemory);
      return false;
    }
    Grid& g = grid_;
    uint32_t size = 0;
    bool dropped = false;
    stack[size++] = 0;
    fillFrom(stack, cap, size, dropped);
    while (dropped) {
      dropped = false;
      for (uint32_t i = 0; i < g.n; i++) {
        if (g.get(i) != EMPTY) continue;
        const uint32_t x = i % static_cast<uint32_t>(g.w), w = static_cast<uint32_t>(g.w);
        const bool outside = (x > 0 && g.get(i - 1) == OUTSIDE) || (x + 1 < w && g.get(i + 1) == OUTSIDE) ||
                             (i >= w && g.get(i - w) == OUTSIDE) || (i + w < g.n && g.get(i + w) == OUTSIDE);
        if (!outside) continue;
        if (size == cap) fillFrom(stack, cap, size, dropped);
        stack[size++] = i;
      }
      fillFrom(stack, cap, size, dropped);
      tuning_.sweeps++;
      if (!tick()) {
        drop(Status::Cancelled);
        return false;
      }
    }
    arena_.setUsed(bottom);
    return true;
  }

  /**
   * The outer boundary of the shape at cell `start` (a cell with the outside to its west), by
   * Moore-neighbour tracing, into `out`. -1 when it has more than `cap` cells.
   */
  int64_t trace(uint32_t startCell, uint32_t* out, size_t cap) const {
    const Grid& g = grid_;
    int cx = static_cast<int>(startCell % static_cast<uint32_t>(g.w)), cy = static_cast<int>(startCell / static_cast<uint32_t>(g.w));
    int back = 4;  // west
    size_t n = 0;
    if (cap < 1) return -1;
    out[n++] = startCell;
    int64_t firstNext = -1;
    for (uint64_t guard = 4 * static_cast<uint64_t>(g.n) + 8; guard > 0; guard--) {
      int k = -1;
      for (int t = 1; t <= 8; t++) {
        const int d = (back + t) & 7;
        if (g.get(static_cast<uint32_t>(cx + DX[d] + (cy + DY[d]) * g.w)) != OUTSIDE) {
          k = d;
          break;
        }
      }
      if (k < 0) break;  // a single cell
      const int nx = cx + DX[k], ny = cy + DY[k];
      const uint32_t next = static_cast<uint32_t>(nx + ny * g.w);
      if (static_cast<uint32_t>(cx + cy * g.w) == startCell) {
        if (firstNext < 0) firstNext = next;
        else if (next == firstNext) break;
      }
      // The new backtrack is the last outside cell looked at, seen from the next cell.
      const int pd = (k + 7) & 7;
      back = DIR[cx + DX[pd] - nx + 1 + (cy + DY[pd] - ny + 1) * 3];
      cx = nx;
      cy = ny;
      if (n == cap) return -1;
      out[n++] = next;
    }
    if (n > 1 && out[n - 1] == startCell) n--;
    return static_cast<int64_t>(n);
  }

  /**
   * A loop's cell centres (outlines()'s xs and ys), worked out from the cells when needed rather
   * than stored: the same doubles, and 16 bytes a cell less (15 §19).
   */
  struct Centres {
    const uint32_t* cells;
    uint32_t n, w;
    double minX, minY, size;
    double x(uint32_t k) const { return minX + (static_cast<double>(cells[k % n] % w) - 1) * size; }
    double y(uint32_t k) const { return minY + (static_cast<double>(cells[k % n] / w) - 1) * size; }
  };

  /** simplifyLoop: Douglas–Peucker on a closed loop; `keep` marks the corners to keep. */
  static void simplify(const Centres& c, double epsilon, uint8_t* keep, uint32_t* spans) {
    const uint32_t n = c.n;
    std::memset(keep, 0, n + 1);
    if (n < 3) {
      for (uint32_t i = 0; i < n; i++) keep[i] = 1;
      return;
    }
    uint32_t far = 0;
    double farD = -1;
    const double x0 = c.x(0), y0 = c.y(0);
    for (uint32_t i = 1; i < n; i++) {
      const double d = fmath::hypot(c.x(i) - x0, c.y(i) - y0);
      if (d > farD) {
        farD = d;
        far = i;
      }
    }
    keep[0] = keep[far] = keep[n] = 1;
    // Index n is index 0 again, closing the loop (Centres takes k % n).
    uint32_t top = 0;
    spans[top++] = 0;
    spans[top++] = far;
    spans[top++] = far;
    spans[top++] = n;
    while (top) {
      const uint32_t b = spans[--top], a = spans[--top];
      if (b - a < 2) continue;
      const double ax = c.x(a), ay = c.y(a), bx = c.x(b), by = c.y(b);
      const double len = fmath::hypot(bx - ax, by - ay);
      uint32_t worst = 0;
      double worstD = -1;
      for (uint32_t i = a + 1; i < b; i++) {
        const double xi = c.x(i), yi = c.y(i);
        const double d = len == 0 ? fmath::hypot(xi - ax, yi - ay) : std::fabs((bx - ax) * (ay - yi) - (ax - xi) * (by - ay)) / len;
        if (d > worstD) {
          worstD = d;
          worst = i;
        }
      }
      if (worstD > epsilon) {
        keep[worst] = 1;
        spans[top++] = a;
        spans[top++] = worst;
        spans[top++] = worst;
        spans[top++] = b;
      }
    }
  }

  /**
   * The kept corners, rounded and without repeats (outlines()): how many, and with `out`, the
   * first `limit` of them written there.
   */
  static uint32_t corners(const Centres& c, const uint8_t* keep, double* out, uint32_t limit) {
    uint32_t count = 0;
    double lastX = 0, lastY = 0, firstX = 0, firstY = 0;
    for (uint32_t k = 0; k < c.n; k++) {
      if (!keep[k]) continue;
      const double vx = jsRound(c.x(k)), vy = jsRound(c.y(k));
      if (count > 0 && lastX == vx && lastY == vy) continue;
      if (count == 0) {
        firstX = vx;
        firstY = vy;
      }
      if (out && count < limit) {
        out[2 * count] = vx;
        out[2 * count + 1] = vy;
      }
      lastX = vx;
      lastY = vy;
      count++;
    }
    // A last corner equal to the first goes. Only one can: the one before it isn't a repeat of it.
    if (count > 1 && firstX == lastX && firstY == lastY) count--;
    return count;
  }

  /**
   * One shape's loop: traced into the free space at the bottom (13 bytes a cell: the cells, the
   * keep flags and Douglas–Peucker's stack), its corners kept at the top below the earlier loops'
   * as [corners][count]. False when the outline stops.
   */
  bool loopAt(uint32_t i) {
    Grid& g = grid_;
    const size_t bottom = arena_.used();
    size_t freeBytes = 0;
    uint32_t* cells = static_cast<uint32_t*>(arena_.freeStart(alignof(uint32_t), freeBytes));
    const size_t cap = freeBytes > 64 ? (freeBytes - 64) / 13 : 0;
    const int64_t traced = trace(i, cells, cap);
    if (traced < 0) {
      arena_.setUsed(bottom);
      drop(Status::OutOfMemory);
      return false;
    }
    const uint32_t n = static_cast<uint32_t>(traced);
    for (uint32_t k = 0; k < n; k++) {
      if (g.get(cells[k]) == TRACED) {  // a shape already traced
        arena_.setUsed(bottom);
        return true;
      }
    }
    for (uint32_t k = 0; k < n; k++) g.set(cells[k], TRACED);
    // Now the n cells are taken, then the rest of the scratch: it fits, as cap was sized for 13
    // bytes a cell.
    arena_.tryAlloc(n * sizeof(uint32_t) + 1, alignof(uint32_t));
    uint8_t* keep = static_cast<uint8_t*>(arena_.tryAlloc(n + 1, 1));
    // Douglas–Peucker's stack: each split takes one span off and puts two on, and at most n - 3
    // corners are added, so it never holds more than n spans (8 bytes each).
    uint32_t* spans = static_cast<uint32_t*>(arena_.tryAlloc((static_cast<size_t>(n) + 2) * 2 * sizeof(uint32_t), alignof(uint32_t)));
    if (!keep || !spans) {
      arena_.setUsed(bottom);
      drop(Status::OutOfMemory);
      return false;
    }
    // Cell centres, simplified to within a cell: that takes out the grid's stair steps.
    const Centres centres{cells, n, static_cast<uint32_t>(g.w), g.minX, g.minY, g.size};
    simplify(centres, g.size, keep, spans);
    const uint32_t c = corners(centres, keep, nullptr, 0);
    double* chunk = static_cast<double*>(arena_.allocTop(16 * static_cast<size_t>(c) + 8, 8));
    if (!chunk) {
      arena_.setUsed(bottom);
      drop(Status::OutOfMemory);
      return false;
    }
    corners(centres, keep, chunk, c);
    std::memcpy(chunk + 2 * c, &c, sizeof c);
    arena_.setUsed(bottom);
    loopCount_++;
    cornerCount_ += c;
    if (!tick()) {
      drop(Status::Cancelled);
      return false;
    }
    return true;
  }

  /** The loops' chunks at the top, in tracing order: (corners, count) into `out`. */
  void chunks(const char* end, const double** corners, uint32_t* counts) const {
    // Walk down from the first chunk (8-aligned below the texts): each is [corners (16 c)][count (8)].
    const char* at = reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(end) / 8 * 8);
    for (uint32_t l = 0; l < loopCount_; l++) {
      uint32_t c;
      std::memcpy(&c, at - 8, sizeof c);
      at -= 8 + 16 * static_cast<size_t>(c);
      corners[l] = reinterpret_cast<const double*>(at);
      counts[l] = c;
    }
  }

  /** Fill, trace, simplify, order; then pack what's kept below the report (15 §18.3). */
  void build() {
    Held& h = *held_;
    if (h.blocked || !grid_.g || !drawn_) {
      // Blocked: nothing will be written. Nothing drawn: "No cutting moves found".
      h.empty = !h.blocked;
      if (h.empty) setSummary(NO_CUTTING_MOVES);
      h.zero = "";
      h.zeroLength = 0;
      h.hasFeed = false;
      h.feedLength = 0;
      arena_.setTop(fixedTop_);
      return;
    }
    if (!tick()) return drop(Status::Cancelled);
    if (!fillOutside()) return;

    // Trace each shape's outer edge. A trace starts at a cell with the outside to its west, the
    // first such cell of a shape in scan order being its lowest, then leftmost. Traced cells are
    // marked, so the shape's other edge cells don't start it again; a trace that does meet a
    // marked cell is of a shape already traced, and is dropped.
    const size_t textsTop = arena_.top();
    for (uint32_t i = 1; i < grid_.n; i++) {
      if ((i & 0xFFFFu) == 0 && !tick()) return drop(Status::Cancelled);
      if (grid_.get(i) > PATH || grid_.get(i - 1) != OUTSIDE) continue;
      if (!loopAt(i)) return;
    }

    // The grid is free now. Order the loops (order()), and pack the kept block:
    // [corners 16 N][loop starts 4 (L + 1)][zero lines][feed], right below the report.
    const size_t bottom = arena_.used();
    const double** loopCorners = static_cast<const double**>(arena_.tryAlloc(loopCount_ * sizeof(double*) + 1, alignof(double*)));
    uint32_t* counts = static_cast<uint32_t*>(arena_.tryAlloc(loopCount_ * sizeof(uint32_t) + 1, alignof(uint32_t)));
    uint8_t* used = static_cast<uint8_t*>(arena_.tryAlloc(loopCount_ + 1, 1));
    if (!loopCorners || !counts || !used) {
      arena_.setUsed(bottom);
      return drop(Status::OutOfMemory);
    }
    chunks(arena_.base() + arena_.size() - textsTop, loopCorners, counts);
    std::memset(used, 0, loopCount_);

    const size_t cornerBytes = 16 * static_cast<size_t>(cornerCount_);
    const size_t startBytes = align8(4 * (static_cast<size_t>(loopCount_) + 1));
    const size_t blockBytes = align8(cornerBytes + startBytes + h.zeroLength + h.feedLength);
    char* const blockEnd = arena_.base() + arena_.size() - fixedTop_;
    char* const gridLow = arena_.base() + arena_.size() - gridTop_;
    char* block = blockEnd - blockBytes;
    const bool inPlace = block >= gridLow;  // the grid's space holds it; else build it at the bottom
    if (!inPlace) {
      block = static_cast<char*>(arena_.tryAlloc(blockBytes, 8));
      if (!block) {
        arena_.setUsed(bottom);
        return drop(Status::OutOfMemory);
      }
    }
    double* corners = reinterpret_cast<double*>(block);
    uint32_t* starts = reinterpret_cast<uint32_t*>(block + cornerBytes);
    char* text = block + cornerBytes + startBytes;

    // Each next loop is the one nearest the current position, started at its nearest corner.
    double atX = hasStart_ ? startX_ : 0, atY = hasStart_ ? startY_ : 0;
    const double startX = atX, startY = atY;
    uint32_t written = 0;
    for (uint32_t step = 0; step < loopCount_; step++) {
      uint32_t bestLoop = 0, bestCorner = 0;
      double best = HUGE_VAL;
      bool found = false;
      for (uint32_t l = 0; l < loopCount_; l++) {
        if (used[l]) continue;
        if (!found) {
          bestLoop = l;
          found = true;
        }
        for (uint32_t k = 0; k < counts[l]; k++) {
          const double d = fmath::hypot(loopCorners[l][2 * k] - atX, loopCorners[l][2 * k + 1] - atY);
          if (d < best) {
            best = d;
            bestLoop = l;
            bestCorner = k;
          }
        }
      }
      used[bestLoop] = 1;
      starts[step] = written;
      const uint32_t c = counts[bestLoop];
      for (uint32_t k = 0; k < c; k++) {
        const uint32_t from = (bestCorner + k) % c;
        corners[2 * written] = loopCorners[bestLoop][2 * from];
        corners[2 * written + 1] = loopCorners[bestLoop][2 * from + 1];
        written++;
      }
      atX = loopCorners[bestLoop][2 * bestCorner];
      atY = loopCorners[bestLoop][2 * bestCorner + 1];
    }
    starts[loopCount_] = written;
    if (h.zeroLength) std::memmove(text, h.zero, h.zeroLength);
    if (h.feedLength) std::memmove(text + h.zeroLength, h.feed, h.feedLength);
    if (!inPlace) std::memmove(blockEnd - blockBytes, block, blockBytes);
    block = blockEnd - blockBytes;
    arena_.setUsed(bottom);
    arena_.setTop(fixedTop_ + blockBytes);

    h.corners = reinterpret_cast<const double*>(block);
    h.loopStarts = reinterpret_cast<const uint32_t*>(block + cornerBytes);
    h.zero = block + cornerBytes + startBytes;
    h.feed = h.zero + h.zeroLength;
    h.loopCount = loopCount_;
    h.cornerCount = cornerCount_;
    h.startX = startX;
    h.startY = startY;
    summarize();
  }

  void setSummary(const char* s) {
    Text t{held_->summary, sizeof held_->summary - 1};
    t.put(s);
    held_->summary[t.len] = '\0';
    held_->summaryLength = static_cast<uint32_t>(t.len);
  }

  /** The tolerance and the summary (buildOutlineCheck); NotSupported where TS's formatFixed throws. */
  void summarize() {
    Held& h = *held_;
    const double cell = grid_.size / scale_;
    const int n = formatSignificant(2 * cell, 2, h.toleranceText, sizeof h.toleranceText);
    h.toleranceLength = n > 0 ? static_cast<uint32_t>(n) : 0;
    h.tolerance = parseDecimal(h.toleranceText, h.toleranceLength);
    double minX = HUGE_VAL, maxX = -HUGE_VAL, minY = HUGE_VAL, maxY = -HUGE_VAL;
    bool safe = isSafeInteger(h.startX) && isSafeInteger(h.startY);
    for (uint32_t k = 0; k < h.cornerCount; k++) {
      const double x = h.corners[2 * k], y = h.corners[2 * k + 1];
      safe = safe && isSafeInteger(x) && isSafeInteger(y);
      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
      if (y < minY) minY = y;
      if (y > maxY) maxY = y;
    }
    if (!safe) return drop(Status::NotSupported);
    const char* units = h.units == Units::In ? "in" : "mm";
    Text t{h.summary, sizeof h.summary - 1};
    if (h.loopCount == 1) {
      t.put("1 outline");
    } else {
      t.count(h.loopCount);
      t.put(" outlines");
    }
    t.put(", ");
    t.count(h.cornerCount);
    t.put(" points, within ");
    t.put(h.toleranceText, h.toleranceLength);
    t.put(" ");
    t.put(units);
    t.put(" \xC2\xB7 X");
    t.fixed(minX, P_);
    t.put("\xE2\x80\x93");
    t.fixed(maxX, P_);
    t.put(" Y");
    t.fixed(minY, P_);
    t.put("\xE2\x80\x93");
    t.fixed(maxY, P_);
    t.put(" ");
    t.put(units);
    h.summary[t.len] = '\0';
    h.summaryLength = static_cast<uint32_t>(t.len);
  }

  ArenaAlloc& arena_;
  const AnalysisOptions& options_;
  bool reportOnly_;
  OutlineTuning& tuning_;
  const GSet g10_, g92_, canned_, comp_;

  Held* held_ = nullptr;
  Status status_ = Status::Ok;
  uint8_t phase_ = 0;
  size_t baseTop_ = 0, fixedTop_ = 0, gridTop_ = 0;
  int32_t first_ = -1;
  uint32_t lineCount_ = 0;
  uint64_t fileBytes_ = 0;
  int P_ = 0;
  double scale_ = 1;

  // The report.
  At at_{0, 0, 0};
  uint8_t source_[kMaxIssues] = {};
  bool haveUnits_ = false, unitsReported_ = false, wcsReported_ = false, haveFirstWcs_ = false;
  UnitsMode units_ = UnitsMode::G21;
  Wcs firstWcs_ = Wcs::G54;
  bool haveEnding_ = false;

  // The words.
  bool feedDone_ = false;
  char* zeroBuf_ = nullptr;
  size_t zeroLength_ = 0, zeroCap_ = 0;
  bool hasStart_ = false;
  double startX_ = 0, startY_ = 0;

  // The path.
  Grid grid_;
  bool hasStockTop_ = false;
  double stockTop_ = 0, zTolerance_ = 0, sampleTolerance_ = 0;
  bool drawn_ = false;
  double lastX_ = 0, lastY_ = 0;

  // The loops.
  uint32_t loopCount_ = 0, cornerCount_ = 0;
};

// ---- Writing (13 §3) ----

class Out {
 public:
  Out(ByteWriter& w, char* buf, size_t cap) : w_(w), buf_(buf), cap_(cap) {}
  void put(const char* p, size_t n) {
    while (n > 0 && !failed) {
      if (len_ == cap_) flush();
      const size_t k = n < cap_ - len_ ? n : cap_ - len_;
      std::memcpy(buf_ + len_, p, k);
      len_ += k;
      p += k;
      n -= k;
    }
  }
  void put(const char* s) { put(s, std::strlen(s)); }
  void fixed(double v, int p) {
    char num[48];
    const int k = formatFixed(v, p, true, num, sizeof num);
    if (k > 0) put(num, static_cast<size_t>(k));
  }
  void flush() {
    if (len_ && !failed && !w_.write(buf_, len_)) failed = true;
    len_ = 0;
  }
  bool failed = false;

 private:
  ByteWriter& w_;
  char* buf_;
  size_t cap_;
  size_t len_ = 0;
};

/**
 * commentSafe on UTF-8 bytes (15 §13.4): parentheses taken out, printable ASCII kept, and one `?`
 * for every other byte that isn't a continuation byte (80–BF), so one per code point.
 */
void putCommentSafe(Out& out, const char* name, size_t n) {
  for (size_t k = 0; k < n; k++) {
    const auto c = static_cast<unsigned char>(name[k]);
    if (c == '(' || c == ')') continue;
    if (c >= 0x20 && c <= 0x7e) out.put(name + k, 1);
    else if (c < 0x80 || c > 0xbf) out.put("?", 1);
  }
}

Status writeFile(const Held& h, ByteWriter& writer, const char* name, size_t nameLength, char* buf, size_t cap) {
  Out out(writer, buf, cap);
  size_t en = 0;
  const char* e = endingText(h.ending, en);
  const char* units = h.units == Units::In ? "in" : "mm";
  const int P = h.precision;
  out.put("(Outline check for ");
  putCommentSafe(out, name, nameLength);
  out.put(", made by G-code Editor)");
  out.put(e, en);
  out.put("(Rapid moves around the outer edge of the cut: the tool centre path, to within ");
  out.put(h.toleranceText, h.toleranceLength);
  out.put(" ");
  out.put(units);
  out.put(")");
  out.put(e, en);
  out.put("(No Z moves, probing, tool changes, or spindle: raise Z to a safe height before running)");
  out.put(e, en);
  if (h.zeroLength) {
    out.put("(Start where the job starts: it sets XY zero there, as the job does)");
    out.put(e, en);
  }
  if (!h.unitsAssumed) {
    out.put(h.units == Units::In ? "G20" : "G21");
    out.put(e, en);
  }
  out.put("G90");
  out.put(e, en);
  if (h.wcs != Wcs::Initial) {
    out.put(wcsName(h.wcs));
    out.put(e, en);
  }
  for (uint32_t k = 0; k < h.zeroLength;) {
    uint32_t end = k;
    while (end < h.zeroLength && h.zero[end] != '\n') end++;
    out.put(h.zero + k, end - k);
    out.put(e, en);
    k = end + 1;
  }
  // Each loop is closed; a single point is a move there, a line is out and back. The first rapid
  // carries the source's rapid F, when it has one.
  bool firstMove = true;
  for (uint32_t l = 0; l < h.loopCount; l++) {
    const uint32_t from = h.loopStarts[l], to = h.loopStarts[l + 1];
    const uint32_t moves = to - from + (to - from > 1 ? 1 : 0);
    for (uint32_t m = 0; m < moves; m++) {
      const uint32_t k = from + (m < to - from ? m : 0);
      out.put("G0 X");
      out.fixed(h.corners[2 * k], P);
      out.put(" Y");
      out.fixed(h.corners[2 * k + 1], P);
      if (firstMove && h.hasFeed) {
        out.put(" F");
        out.put(h.feed, h.feedLength);
      }
      firstMove = false;
      out.put(e, en);
    }
  }
  out.put("G0 X");
  out.fixed(h.startX, P);
  out.put(" Y");
  out.fixed(h.startY, P);
  out.put(" (back to the start)");
  out.put(e, en);
  out.put("M2");
  out.put(e, en);
  out.flush();
  return out.failed ? Status::WriteError : Status::Ok;
}

void fillResult(const Held& h, OutlineResult& r) {
  std::memset(&r, 0, sizeof r);
  r.empty = h.empty;
  r.corners = h.corners;
  r.loopStarts = h.loopStarts;
  r.loopCount = h.loopCount;
  r.precision = h.precision;
  r.tolerance = h.tolerance;
  std::memcpy(r.summary, h.summary, sizeof r.summary);
  r.summaryLength = h.summaryLength;
}

void fillReport(const Held& h, Report& report, size_t arenaUsed) {
  report = Report{h.issues, h.issueCount, 0, h.blocked, h.warnings, arenaUsed};
}

/** Remembers the line last read, for B-LINE-TOO-LONG. */
class LastLine final : public LineReader {
 public:
  explicit LastLine(LineReader& r) : r_(r) {}
  bool next(Line& out) override {
    if (!r_.next(out)) return false;
    last = out;
    return true;
  }
  bool rewind() override { return r_.rewind(); }
  bool failed() const override { return r_.failed(); }
  Line last{};

 private:
  LineReader& r_;
};

/** The standalone run: analyse() with the outline, from the file's first line. */
Status run(LineReader& reader, const OutlineOptions& options, ArenaAlloc& arena, bool reportOnly, OutlineTuning& tuning, Report& report,
           const Held*& held) {
  report = Report{nullptr, 0, 0, false, false, 0};
  held = nullptr;
  Analysis* a = arena.array<Analysis>(1);
  if (!a) return Status::OutOfMemory;
  AnalysisOptions ao;
  ao.maxLineLength = options.maxLineLength > 65535 ? 65535 : options.maxLineLength;
  ao.progress = options.progress;
  ao.progressContext = options.progressContext;
  ao.outline = true;
  if (!reader.rewind()) return Status::ReadError;
  LastLine last(reader);
  const Status s = analyseOutline(last, ao, arena, nullptr, *a, reportOnly, tuning);
  if (s == Status::LineTooLong) {
    // The one rule TS doesn't have (15 §4.2): the report blocks on that line.
    struct Long {
      Issue issue;
      char text[96];
    };
    Long* lt = arena.array<Long>(1);
    if (!lt) return Status::OutOfMemory;
    const Line& l = last.last;
    const uint32_t bom = l.index == 0 && a->utf8Bom ? 3 : 0;
    Text t{lt->text, sizeof lt->text};
    t.put("Line too long: more than ");
    t.count(ao.maxLineLength);
    t.put(" bytes");
    lt->issue = Issue{l.index + 1, l.offset + bom, l.length - bom, Severity::Blocking, "B-LINE-TOO-LONG", Str{lt->text, static_cast<uint32_t>(t.len)}, false};
    report = Report{&lt->issue, 1, 0, true, false, arena.peak()};
    return Status::Ok;
  }
  if (s != Status::Ok) return s;
  if (a->outlineStatus != Status::Ok) return a->outlineStatus;
  held = static_cast<const Held*>(a->outline);
  fillReport(*held, report, arena.peak());
  return Status::Ok;
}

}  // namespace

// ---- analyse() with the outline ----

Status analyseOutline(LineReader& reader, const AnalysisOptions& options, ArenaAlloc& arena, AnalysisSink* sink, Analysis& result,
                      bool reportOnly, OutlineTuning& tuning) {
  Builder b(arena, options, reportOnly, tuning);
  const Status s = analyseWith(reader, options, arena, sink, &b, result);
  b.finish(result, s);
  return s;
}

// ---- The interface (outline.h) ----

Status checkOutline(LineReader& reader, const OutlineOptions& options, Arena arenaIn, Report& report) {
  ArenaAlloc arena(arenaIn);
  OutlineTuning tuning;
  const Held* held = nullptr;
  return run(reader, options, arena, true, tuning, report, held);
}

Status outlineCheckTuned(LineReader& reader, ByteWriter& writer, const char* sourceName, size_t nameLength, const OutlineOptions& options,
                         Arena arenaIn, Report& report, OutlineResult& result, OutlineTuning& tuning) {
  std::memset(&result, 0, sizeof result);
  ArenaAlloc arena(arenaIn);
  const Held* held = nullptr;
  const Status s = run(reader, options, arena, false, tuning, report, held);
  if (s != Status::Ok) return s;
  if (report.blocked) return Status::Blocked;
  fillResult(*held, result);
  if (held->empty) return Status::Ok;
  // The write buffer from the free space between the analysis and the outline.
  const size_t cap = arena.free() < 4096 ? arena.free() : 4096;
  char* buf = cap >= 256 ? static_cast<char*>(arena.tryAlloc(cap, 1)) : nullptr;
  if (!buf) return Status::OutOfMemory;
  return writeFile(*held, writer, sourceName, nameLength, buf, cap);
}

Status outlineCheck(LineReader& reader, ByteWriter& writer, const char* sourceName, size_t nameLength, const OutlineOptions& options,
                    Arena arena, Report& report, OutlineResult& result) {
  OutlineTuning tuning;
  return outlineCheckTuned(reader, writer, sourceName, nameLength, options, arena, report, result, tuning);
}

Status outlineReport(const Analysis& analysis, Report& report) {
  report = Report{nullptr, 0, 0, false, false, 0};
  if (analysis.outlineStatus != Status::Ok || !analysis.outline) return analysis.outlineStatus;
  fillReport(*static_cast<const Held*>(analysis.outline), report, analysis.arenaPeak);
  return Status::Ok;
}

Status outlineLoops(const Analysis& analysis, OutlineResult& result) {
  std::memset(&result, 0, sizeof result);
  if (analysis.outlineStatus != Status::Ok || !analysis.outline) return analysis.outlineStatus;
  const Held& h = *static_cast<const Held*>(analysis.outline);
  if (h.blocked) return Status::Blocked;
  fillResult(h, result);
  return Status::Ok;
}

Status writeOutline(const Analysis& analysis, ByteWriter& writer, const char* sourceName, size_t nameLength, Arena arena,
                    OutlineResult& result) {
  const Status s = outlineLoops(analysis, result);
  if (s != Status::Ok || result.empty) return s;
  const Held& h = *static_cast<const Held*>(analysis.outline);
  // The free space between the analysis (at the bottom) and the outline (at the top).
  char* const base = static_cast<char*>(arena.base);
  const size_t lo = analysis.arenaUsed, hi = arena.size >= analysis.outlineHeld ? arena.size - analysis.outlineHeld : 0;
  const uintptr_t held = reinterpret_cast<uintptr_t>(&h), from = reinterpret_cast<uintptr_t>(base);
  if (held < from || held >= from + arena.size || hi < lo) return Status::OutOfMemory;  // not the arena analyse() had
  const size_t freeBytes = hi - lo;
  const size_t cap = freeBytes < 4096 ? freeBytes : 4096;
  if (cap < 256) return Status::OutOfMemory;
  return writeFile(h, writer, sourceName, nameLength, base + lo, cap);
}

size_t outlineCheckName(const char* name, size_t length, char* out, size_t capacity) {
  size_t dot = length;
  for (size_t k = length; k > 0; k--) {
    if (name[k - 1] == '.') {
      dot = k - 1;
      break;
    }
  }
  const bool ext = dot < length && dot > 0;
  static const char kInsert[] = ".outline";
  static const char kAppend[] = ".outline.nc";
  const size_t addLength = ext ? sizeof kInsert - 1 : sizeof kAppend - 1;
  const size_t n = length + addLength;
  if (capacity <= n) return n;
  if (ext) {
    std::memcpy(out, name, dot);
    std::memcpy(out + dot, kInsert, addLength);
    std::memcpy(out + dot + addLength, name + dot, length - dot);
  } else {
    std::memcpy(out, name, length);
    std::memcpy(out + length, kAppend, addLength);
  }
  out[n] = '\0';
  return n;
}

}  // namespace gcode
