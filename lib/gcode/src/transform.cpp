// Rotate and Flip (specs/15 §5.3, C2): the catalog operations of ops/catalog.ts, which run
// ops/rotate90.ts's makeTransform on the enabled form (ops/enabled-form.ts).
//
//   check():  analyse() on the enabled form, with a FinalPass that classifies each line on
//             pass 2 (ROTATE_RULES over classifyLines), finds the file-level issues, Flip's
//             issues and the overflows, then builds the capped report (report.h).
//   apply():  check() again, then a third pass that writes every line: the `;OFF ` prefix, the
//             line with its X/Y/I/J words (and for a mirror its G2/G3 word) and stock bounds
//             replaced, the ending.
#include "gcode/transform.h"

#include <cmath>
#include <cstring>

#include "analyse_internal.h"
#include "arena.h"
#include "gcode/num.h"
#include "gcode/tokens.h"
#include "report.h"
#include "rewrite.h"
#include "simulator.h"
#include "structure.h"
#include "vocabulary.h"

namespace gcode {
namespace {

const uint32_t XY = (1u << ('X' - 'A')) | (1u << ('Y' - 'A'));
const uint32_t XYIJ = XY | (1u << ('I' - 'A')) | (1u << ('J' - 'A'));
const uint32_t AXES = XY | (1u << ('Z' - 'A')) | (1u << ('A' - 'A')) | (1u << ('B' - 'A')) | (1u << ('C' - 'A')) | (1u << ('U' - 'A')) |
                      (1u << ('V' - 'A')) | (1u << ('W' - 'A'));

bool hasG(const LineState& st, int key) {
  const int b = vocab::gIndex(key);
  if (b < 0) return false;
  return b < 32 ? ((st.gLo >> b) & 1u) != 0 : ((st.gHi >> (b - 32)) & 1u) != 0;
}
bool hasM(const LineState& st, int key) {
  const int b = vocab::mIndex(key);
  return b >= 0 && ((st.mBits >> b) & 1u) != 0;
}
bool anyG(const LineState& st, const int* keys, int n) {
  for (int k = 0; k < n; k++) {
    if (hasG(st, keys[k])) return true;
  }
  return false;
}

const int PROBES[] = {382, 383, 384, 385};
const int CANNED[] = {730, 760, 810, 820, 830, 840, 850, 860, 870, 880, 890};
/** G-codes that make a line something other than a motion line (their words aren't arc words). */
const int NON_MOTION[] = {40, 100, 280, 281, 300, 301, 530, 920, 921, 922, 923};
const int P_OWNER_M[] = {560, 620, 630, 640, 650};

bool isCannedKey(int key) {
  for (int k : CANNED) {
    if (k == key) return true;
  }
  return false;
}

bool fail(ArenaAlloc& a) {
  a.failed = true;
  return false;
}

// ---- Messages (ops/rotate90.ts, ops/flip.ts), byte for byte ----

const char* const OVERFLOW_REASON = "Coordinate too large for its precision";
const char* const W_FLIP_REASON =
    "A mirror image reverses the cutting direction: climb milling becomes conventional, and conventional becomes climb. "
    "Flip and reverse cuts runs closed loops and whole-turn helices the original way round; ramps and moves between passes stay mirrored";
const char* const NO_STOCK_DEPTH_REASON =
    "The file has no (Stock: \xE2\x80\xA6) comment, so through cuts can't be confirmed: anything that doesn't cut all the way through will "
    "cut into the opposite side after the flip";

// Rule ids: kept issues point at these.
const char* const B_OVERFLOW = "B-OVERFLOW";

// ---- The enabled form (ops/enabled-form.ts) ----

/**
 * The file's lines with every `;OFF ` prefix removed (11 §6). Lines map one to one; each line's
 * offset is that of its text after the prefix, so an issue's line, quoted with its prefix as TS
 * quotes it, starts at `offset - prefix()`.
 */
class EnabledForm final : public LineReader {
 public:
  EnabledForm(LineReader& r, char* buf, size_t cap) : r_(r), buf_(buf), cap_(cap) {}

  bool next(Line& out) override {
    if (!r_.next(orig_)) return false;
    const auto* b = reinterpret_cast<const unsigned char*>(orig_.data);
    const uint32_t bom = orig_.index == 0 && orig_.length >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf ? 3 : 0;
    prefix_ = offPrefixLength(orig_.data + bom, orig_.length - bom);
    out = orig_;
    if (prefix_ == 0) return true;
    const uint32_t rest = orig_.length - bom - prefix_;
    if (bom == 0) {
      out.data = orig_.data + prefix_;
    } else if (bom + rest <= cap_) {
      // The BOM, then the line without its prefix: analyse() skips the BOM itself.
      std::memcpy(buf_, orig_.data, bom);
      std::memcpy(buf_ + bom, orig_.data + bom + prefix_, rest);
      out.data = buf_;
    } else {
      prefix_ = 0;  // too long either way: analyse() reports the line
      return true;
    }
    out.length = bom + rest;
    out.offset = orig_.offset + prefix_;
    return true;
  }
  bool rewind() override { return r_.rewind(); }
  bool failed() const override { return r_.failed(); }

  /** The current line's `;OFF ` prefix length. */
  uint32_t prefix() const { return prefix_; }
  /** The current line as read from the file. */
  const Line& original() const { return orig_; }

 private:
  LineReader& r_;
  char* buf_;
  size_t cap_;
  Line orig_{};
  uint32_t prefix_ = 0;
};

// ---- The checks (validate) ----

/** Flip's cut groups (flip.ts CutGroup): the cutting of one operation or of the rest. */
struct CutGroup {
  Str name;
  uint32_t line;  // 1-based line to report, with its text's place
  uint64_t offset;
  uint32_t length;
  bool toolChange;
  double deepest;  // lowest Z at feed; +inf with none
  uint32_t deepestLine;
  uint64_t deepestOffset;
  uint32_t deepestLength;
};

/** Where an issue's line is: 1-based number, and its text's offset and length. */
struct At {
  uint32_t line;
  uint64_t offset;
  uint32_t length;
};

class Checker final : public FinalPass {
 public:
  Checker(TransformKind kind, ArenaAlloc& arena, EnabledForm& form, IssueCollector& issues, char* scratch, size_t scratchCap)
      : kind_(kind), mirror_(kind == TransformKind::FlipH || kind == TransformKind::FlipV), arena_(arena), form_(form), issues_(issues),
        scratch_(scratch), scratchCap_(scratchCap) {}

  bool begin(const Analysis& a, const PassOneFacts& facts) override {
    a_ = &a;
    const int g10 = vocab::gIndex(100);
    const bool hasG10 = g10 < 32 ? ((facts.gLo >> g10) & 1u) != 0 : ((facts.gHi >> (g10 - 32)) & 1u) != 0;
    hasSetup = setupTransform(kind_, a, hasG10, setup);
    outside_ = CutGroup{{"", 0}, 1, 0, 0, false, HUGE_VAL, 0, 0, 0};
    return true;
  }

  bool line(const Line& line, const Token* tokens, uint16_t tokenCount, const LineState& st) override {
    const uint32_t i = line.index;
    const At at{i + 1, line.offset - form_.prefix(), line.length + form_.prefix()};
    if (i == 0) first_ = at;
    text_ = line.data;
    n_ = line.length;
    tokens_ = tokens;
    tokenCount_ = tokenCount;
    st_ = &st;
    if (!classify(at)) return false;
    if (!fileLevel(st, at)) return false;
    if (mirror_ && !flipIssues(st, at)) return false;
    return overflow(st, at);
  }

  /** After pass 2: the through-cut warnings (flip.ts throughCutIssues). */
  bool finish() { return !mirror_ || throughCuts(); }

  bool hasSetup = false;
  TransformSetup setup{};

 private:
  // ---- Classification (classify.ts classifyLines with ROTATE_RULES) ----

  struct Verdict {
    Severity severity;
    const char* ruleId;
    size_t start, length;
  };

  /** Starts a verdict's reason in the line's scratch. */
  TextBuilder reason() { return TextBuilder(scratch_ + used_, scratchCap_ - used_); }

  /** Keeps a verdict, unless the line already has one with the same reason (classifyLines' emit). */
  void verdict(Severity s, const char* ruleId, const TextBuilder& b) {
    if (b.len >= b.cap) {
      tooLong_ = true;
      return;
    }
    for (int k = 0; k < verdictCount_; k++) {
      if (verdicts_[k].length == b.len && std::memcmp(scratch_ + verdicts_[k].start, b.buf, b.len) == 0) return;
    }
    if (verdictCount_ >= kMaxVerdicts) {
      tooLong_ = true;
      return;
    }
    verdicts_[verdictCount_++] = Verdict{s, ruleId, used_, b.len};
    used_ += b.len;
  }
  void verdict(Severity s, const char* ruleId, const char* text) {
    TextBuilder b = reason();
    b.put(text);
    verdict(s, ruleId, b);
  }
  void blocking(const char* ruleId, const char* text) { verdict(Severity::Blocking, ruleId, text); }
  void warning(const char* ruleId, const char* text) { verdict(Severity::Warning, ruleId, text); }

  Str tokenText(const Token& t) const { return Str{text_ + t.start, t.length}; }
  Str valueText(const Token& t) const { return Str{text_ + t.valueStart, static_cast<uint32_t>(t.start + t.length - t.valueStart)}; }

  /** offendingToken: the first token that made the tokenizer or vocabulary flag the line. */
  Str offendingToken() const {
    for (uint16_t k = 0; k < tokenCount_; k++) {
      const Token& t = tokens_[k];
      bool match = t.kind == TokenKind::Raw;
      if (t.kind == TokenKind::Comment && t.style == CommentStyle::Paren) match = text_[t.start + t.length - 1] != ')';
      if (t.kind == TokenKind::Word) {
        const Str v = valueText(t);
        match = !vocab::letterRecognized(t.letter) || (t.letter == 'G' && vocab::gIndex(codeKey(v.data, v.length)) < 0);
      }
      if (match) return tokenText(t);
    }
    return trimmedBlanks(text_, n_);
  }

  /** mWord: the first unrecognized M word, or "M". */
  Str mWord() const {
    for (uint16_t k = 0; k < tokenCount_; k++) {
      const Token& t = tokens_[k];
      if (t.kind != TokenKind::Word || t.letter != 'M') continue;
      const Str v = valueText(t);
      if (vocab::mIndex(codeKey(v.data, v.length)) < 0) return tokenText(t);
    }
    return Str{"M", 1};
  }

  void syntax() {
    TextBuilder b = reason();
    b.put("Unrecognized G-code or syntax: ");
    b.put(offendingToken());
    verdict(Severity::Blocking, "B-SYNTAX", b);
  }
  void parametric() { blocking("B-PARAMETRIC", "Parametric / subroutine / checksum syntax not supported"); }
  void unknownPosition() { blocking("B-UNKNOWN-POS", "XY move from an unknown position"); }

  void flagRule(int b) {
    const LineState& st = *st_;
    switch (b) {
      case 0: case 1: case 5: case 6: case 7: syntax(); break;  // MalformedNumber, UnclosedComment, UnrecognizedSyntax, UnknownLetter, UnknownGCode
      case 2: blocking("B-JOG", "Jog command with axis moves"); break;
      case 3: case 4: parametric(); break;  // ParametricSyntax, Checksum
      case 8:  // UnknownMCode
        if ((st.letters & AXES) == 0) {
          TextBuilder r = reason();
          r.put("Unrecognized M-code ");
          r.put(mWord());
          r.put(" will pass through unchanged");
          verdict(Severity::Warning, "W1", r);
        } else {
          blocking("B-MCODE-MOTION", "Unrecognized M-code on a motion line");
        }
        break;
      case 9: blocking("B-INVALID", "Invalid line: the same word appears twice"); break;
      case 10: blocking(B_OVERFLOW, OVERFLOW_REASON); break;
      case 11: blocking("B-INVALID", "Invalid line: two G-codes from the same modal group"); break;
      case 12: warning("W4", "Axis words before any motion command; assuming G0"); break;
      case 13: warning("W3", "File doesn't set G90/G91 before moving; assuming absolute (G90)"); break;
      case 14: warning("W2", "File doesn't set units (G20/G21); assuming mm"); break;
      case 15: case 16: unknownPosition(); break;  // IncrementalFromUnknown, UnknownStart
      case 17: {  // ArcInvalid
        TextBuilder r = reason();
        r.put("Invalid arc: ");
        if (st.arc.state == ArcState::Invalid) r.put(st.arc.reason, st.arc.reasonLength);
        else r.put("P (number of turns) isn't supported on arcs");
        verdict(Severity::Blocking, "B-ARC", r);
        break;
      }
      case 18: blocking("B-ARC-PLANE", "Arc not in the XY plane (G18/G19)"); break;
      default: break;
    }
  }

  /** arcWordRule: I, J and R belong to a G2/G3 motion line (06 §5.4). */
  bool arcLine() const {
    const Motion m = st_->motion;
    return (m == Motion::G2 || m == Motion::G3) && !anyG(*st_, NON_MOTION, 11);
  }

  void pWordRule() {
    const LineState& st = *st_;
    bool pOwnerM = false;
    for (int k : P_OWNER_M) pOwnerM = pOwnerM || hasM(st, k);
    const int owners = (hasG(st, 40) ? 1 : 0) + (hasG(st, 100) ? 1 : 0) + (anyG(st, PROBES, 4) ? 1 : 0) + (pOwnerM ? 1 : 0);
    const int explicitArc = hasG(st, 20) || hasG(st, 30) ? 1 : 0;
    const int candidates = owners + explicitArc;
    if (candidates == 0) blocking("B-P", "P word not supported here");
    else if (candidates > 1) blocking("B-P-AMBIGUOUS", "Ambiguous P word");
    else if (explicitArc) blocking("B-ARC", "Invalid arc: P (number of turns) isn't supported on arcs");
  }

  void letterRule(int b) {
    const LineState& st = *st_;
    switch (b + 'A') {
      case 'A': case 'B': case 'C': case 'U': case 'V': case 'W': blocking("B-AXIS", "Rotary / extra axis present"); break;
      case 'E':
        if (!hasM(st, 670) && !hasM(st, 680)) blocking("B-EQ", "E word without M67/M68");
        break;
      case 'Q':
        if (!hasM(st, 670) && !hasM(st, 680)) blocking("B-EQ", "Q word without M67/M68");
        break;
      case 'H':
        if (!hasG(st, 430)) blocking("B-H", "H word without G43");
        break;
      case 'I': case 'J':
        if (!arcLine()) blocking("B-IJ", "I/J outside an XY arc");
        break;
      case 'K':
        // The Z part of an arc centre: unused in G17, and unchanged by a rotation about Z.
        if (st.plane != Plane::G17 || !arcLine()) blocking("B-K", "K word not supported");
        break;
      case 'L':
        if (!hasG(st, 100)) blocking("B-L", "L word not supported");
        break;
      case 'P': pWordRule(); break;
      case 'R':
        if (!arcLine()) blocking("B-R", "R outside an arc");
        break;
      default: break;  // F G M N S T X Y Z allowed; D and O are flagged UnknownLetter
    }
  }

  /** g10Rule: a G10 L20 before the first XY motion sets work zero; any other G10 blocks. */
  void g10Rule(uint32_t index) {
    const int32_t first = a_->firstXYMotion;
    const bool beforeMotion = first < 0 || static_cast<int32_t>(index) < first;
    if (beforeMotion) {
      // /^L[ \t]*0*20(\.0*)?$/i on the first L word's text.
      for (uint16_t k = 0; k < tokenCount_; k++) {
        const Token& t = tokens_[k];
        if (t.kind != TokenKind::Word || t.letter != 'L') continue;
        const char* p = text_ + t.start;
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
        if (ok) return;
        break;
      }
    }
    blocking("B-G10", beforeMotion ? "Work offset change (G10) other than L20" : "Work offset change (G10) mid-file");
  }

  void gRule(int key, uint32_t index) {
    const bool xy = (st_->letters & XY) != 0;
    switch (key) {
      case 100: g10Rule(index); break;
      case 280: case 300:
        if (xy) blocking("B-G28", "Home via intermediate XY point");
        break;
      case 382: case 383: case 384: case 385:
        if (xy) blocking("B-G38", "XY probing move");
        break;
      case 410: case 420: blocking("B-COMP", "Cutter compensation not supported"); break;
      case 530:
        if (xy) blocking("B-G53", "Machine-coordinate XY move");
        break;
      case 901: blocking("B-G90.1", "Absolute IJ mode not supported"); break;
      case 920: case 921: case 922: case 923: blocking("B-G92", "Coordinate offset (G92) changes the coordinate system mid-file"); break;
      default:
        if (isCannedKey(key)) blocking("B-CANNED", "Canned cycles not supported");
        break;
    }
  }

  /** ownerRule: axis words under a modal canned cycle, G80, or a modal probe (06 §5.2). */
  void ownerRule() {
    const LineState& st = *st_;
    if (st.owner == Owner::Motion) {
      if (st.motion == Motion::G80) blocking("B-G80", "Axis words with no motion mode (G80)");
      else if (static_cast<int>(st.motion) >= static_cast<int>(Motion::G73) && !anyG(st, CANNED, 11)) blocking("B-CANNED", "Canned cycles not supported");
    }
    if (st.owner == Owner::G38 && (st.letters & XY) != 0 && !anyG(st, PROBES, 4)) blocking("B-G38", "XY probing move");
  }

  bool classify(const At& at) {
    const LineState& st = *st_;
    verdictCount_ = 0;
    used_ = 0;
    const uint32_t flags = st.parseFlags | st.flags;
    if (flags) {
      for (int b = 0; b <= 18; b++) {
        if (flags & (1u << b)) flagRule(b);
      }
    }
    if (st.letters) {
      for (int b = 0; b < 26; b++) {
        if (st.letters & (1u << b)) letterRule(b);
      }
      for (int b = 0; b < 64; b++) {
        const bool set = b < 32 ? ((st.gLo >> b) & 1u) != 0 : ((st.gHi >> (b - 32)) & 1u) != 0;
        if (set) gRule(vocab::gKeyAt(b), at.line - 1);
      }
      // Every recognized M-code is allowed (06 §5.3).
      ownerRule();
    }
    if (tooLong_) return fail(arena_);
    overflowed_ = false;
    for (int k = 0; k < verdictCount_; k++) {
      const Verdict& v = verdicts_[k];
      if (v.ruleId == B_OVERFLOW) overflowed_ = true;
      if (!issues_.add(0, v.severity, v.ruleId, scratch_ + v.start, v.length, at.line, at.offset, at.length, false)) return false;
    }
    return true;
  }

  // ---- File-level issues (rotate90.ts fileLevelIssues) ----

  bool fileLevel(const LineState& st, const At& at) {
    if (!st.isXYMotion || (unitsReported_ && wcsReported_)) return true;
    if (!haveUnits_) {
      units_ = st.units;
      haveUnits_ = true;
    }
    if (!unitsReported_ && st.units != units_) {
      unitsReported_ = true;
      if (!add(1, Severity::Blocking, "B-UNITS", "Mixed units: XY motion in both mm and inches", at)) return false;
    }
    if (st.wcs == Wcs::Initial && !haveFirstInitial_) {
      firstInitial_ = at;
      haveFirstInitial_ = true;
    }
    // Moves before any G54–G59 count as G54, the Grbl/FluidNC power-on default.
    const Wcs effective = st.wcs == Wcs::Initial ? Wcs::G54 : st.wcs;
    if (!haveFirstWcs_) {
      firstWcs_ = effective;
      haveFirstWcs_ = true;
    }
    if (!wcsReported_ && effective != firstWcs_) {
      wcsReported_ = true;
      return add(1, Severity::Blocking, "B-WCS", "Multiple work coordinate systems in one file", at);
    }
    if (!assumptionReported_ && st.wcs == Wcs::G54 && haveFirstInitial_) {
      assumptionReported_ = true;
      return add(1, Severity::Warning, "W5", "XY move before any work coordinate system is selected; assuming G54, which the file selects later",
                 firstInitial_);
    }
    return true;
  }

  // ---- Flip's issues (rotate90.ts flipIssues, flip.ts cutGroups) ----

  bool flipIssues(const LineState& st, const At& at) {
    if (!flipReported_ && st.isXYMotion && st.motion != Motion::G0) {
      flipReported_ = true;
      if (!issues_.add(2, Severity::Warning, "W-FLIP", W_FLIP_REASON, std::strlen(W_FLIP_REASON), at.line, at.offset, at.length, true)) return false;
    }
    // Cut groups.
    Str name{"", 0};
    const LineMarker marker = lineMarker(text_, n_, name);
    if (marker == LineMarker::BeginOperation) {
      CutGroup g{arena_.copy(name.data, name.length), at.line, at.offset, at.length, false, HUGE_VAL, 0, 0, 0};
      if (!ops_.push(arena_, g)) return false;
      open_ = true;
      return !arena_.failed;
    }
    if (marker == LineMarker::FinishOperation) {
      open_ = false;
      return true;
    }
    if (open_ && marker == LineMarker::ToolChange) ops_.back().toolChange = true;
    const Motion m = st.motion;
    if ((m != Motion::G1 && m != Motion::G2 && m != Motion::G3) || st.owner != Owner::Motion) return true;
    const double z = st.after.z;
    CutGroup& g = open_ ? ops_.back() : outside_;
    if (!std::isnan(z) && z < g.deepest) {
      g.deepest = z;
      g.deepestLine = at.line;
      g.deepestOffset = at.offset;
      g.deepestLength = at.length;
    }
    return true;
  }

  /** `Z` + formatDecimal(z, 4). */
  static void zText(TextBuilder& b, double z) {
    char num[48];
    const int n = formatDecimal(z, 4, false, num, sizeof num);
    b.put("Z");
    if (n > 0) b.put(num, static_cast<size_t>(n));
  }

  bool notThrough(Str name, double deepest, const char* whereLead, double whereZ, uint32_t line, uint64_t offset, uint32_t length) {
    TextBuilder b = reason();
    b.put(name);
    b.put(" stops at ");
    zText(b, deepest);
    b.put(", above ");
    b.put(whereLead);
    zText(b, whereZ);
    b.put("): it isn't a through cut, so after the flip it cuts into the opposite side");
    if (b.len >= b.cap) return fail(arena_);
    return issues_.add(2, Severity::Warning, "W-NOT-THROUGH", b.buf, b.len, line, offset, length, false);
  }

  bool throughCuts() {
    if (!a_->extents.present) return true;  // nothing will flip
    used_ = 0;
    const double tolerance = 1 / pow10(a_->precision);  // one unit in the last place
    // The operations that count: no tool change inside, and some cutting.
    uint32_t opCount = 0;
    for (uint32_t k = 0; k < ops_.size; k++) {
      if (!ops_.data[k].toolChange && ops_.data[k].deepest != HUGE_VAL) opCount++;
    }
    const auto counts = [](const CutGroup& g) { return !g.toolChange && g.deepest != HUGE_VAL; };
    if (a_->stockCommentCount) {
      const StockComment& s = a_->stockComments[a_->stockCommentCount - 1];
      const double bottom = parseDecimal(s.values[4].text.data, s.values[4].text.length);
      const double top = parseDecimal(s.values[5].text.data, s.values[5].text.length);
      const auto shallow = [&](const CutGroup& g) { return g.deepest > bottom + tolerance && g.deepest < top - tolerance; };
      const char* where = "the bottom of the stock (";
      for (uint32_t k = 0; k < ops_.size; k++) {
        const CutGroup& g = ops_.data[k];
        if (counts(g) && shallow(g) && !notThrough(g.name, g.deepest, where, bottom, g.line, g.offset, g.length)) return false;
      }
      if (outside_.deepest != HUGE_VAL && shallow(outside_)) {
        const char* label = opCount ? "G-code outside any operation" : "The toolpath";
        const Str name{label, static_cast<uint32_t>(std::strlen(label))};
        return notThrough(name, outside_.deepest, where, bottom, outside_.deepestLine, outside_.deepestOffset, outside_.deepestLength);
      }
      return true;
    }
    double deepest = outside_.deepest;
    for (uint32_t k = 0; k < ops_.size; k++) {
      if (counts(ops_.data[k]) && ops_.data[k].deepest < deepest) deepest = ops_.data[k].deepest;
    }
    if (deepest == HUGE_VAL) return true;
    if (!issues_.add(2, Severity::Warning, "W-NO-STOCK-DEPTH", NO_STOCK_DEPTH_REASON, std::strlen(NO_STOCK_DEPTH_REASON), first_.line, first_.offset,
                     first_.length, false)) {
      return false;
    }
    for (uint32_t k = 0; k < ops_.size; k++) {
      const CutGroup& g = ops_.data[k];
      if (counts(g) && g.deepest > deepest + tolerance && !notThrough(g.name, g.deepest, "the deepest cut in the file (", deepest, g.line, g.offset, g.length)) {
        return false;
      }
    }
    return true;
  }

  // ---- Overflows (rotate90.ts overflowIssues) ----

  bool isFixed(uint32_t i) {
    while (fixedAt_ < a_->fixedMoveCount && a_->fixedMoves[fixedAt_].line < i) fixedAt_++;
    return fixedAt_ < a_->fixedMoveCount && a_->fixedMoves[fixedAt_].line == i;
  }
  bool isStockLine(uint32_t i) {
    while (stockAt_ < a_->stockCommentCount && a_->stockComments[stockAt_].line < i) stockAt_++;
    return stockAt_ < a_->stockCommentCount && a_->stockComments[stockAt_].line == i;
  }

  bool overflow(const LineState& st, const At& at) {
    if (!hasSetup) return true;
    const uint32_t i = at.line - 1;
    // rewritesWords: X/Y/I/J on motion lines (a preamble G10 L20 stays as written), and for a
    // mirror G2/G3, which can stand on a line of their own.
    const bool words = ((st.letters & XYIJ) != 0 && (st.owner == Owner::None || st.owner == Owner::Motion) && !isFixed(i)) ||
                       (mirror_ && (hasG(st, 20) || hasG(st, 30)));
    const bool stock = isStockLine(i);
    if ((!words && !stock) || overflowed_) return true;
    Edit edits[5];
    int count = 0;
    const bool fits = (!words || wordEdits(text_, n_, setup, st.distance == Distance::G91, edits, count)) &&
                      (!stock || stockEdits(text_, n_, i, setup, nullptr));
    return fits || add(3, Severity::Blocking, B_OVERFLOW, OVERFLOW_REASON, at);
  }

  bool add(uint8_t source, Severity s, const char* ruleId, const char* reason, const At& at) {
    return issues_.add(source, s, ruleId, reason, std::strlen(reason), at.line, at.offset, at.length, false);
  }

  static constexpr int kMaxVerdicts = 160;

  TransformKind kind_;
  bool mirror_;
  ArenaAlloc& arena_;
  EnabledForm& form_;
  IssueCollector& issues_;
  char* scratch_;
  size_t scratchCap_;
  const Analysis* a_ = nullptr;

  // The current line.
  const char* text_ = "";
  uint32_t n_ = 0;
  const Token* tokens_ = nullptr;
  uint16_t tokenCount_ = 0;
  const LineState* st_ = nullptr;
  Verdict verdicts_[kMaxVerdicts];
  int verdictCount_ = 0;
  size_t used_ = 0;
  bool tooLong_ = false;
  bool overflowed_ = false;
  At first_{1, 0, 0};

  // File-level state.
  bool haveUnits_ = false, unitsReported_ = false, wcsReported_ = false, assumptionReported_ = false;
  UnitsMode units_ = UnitsMode::G21;
  bool haveFirstWcs_ = false, haveFirstInitial_ = false;
  Wcs firstWcs_ = Wcs::G54;
  At firstInitial_{0, 0, 0};

  // Flip.
  bool flipReported_ = false;
  bool open_ = false;
  Vec<CutGroup> ops_;
  CutGroup outside_{};

  uint32_t fixedAt_ = 0, stockAt_ = 0;
};

/** What check() leaves for apply(). */
struct Checked {
  Analysis analysis;
  bool hasSetup;
  TransformSetup setup;
};

uint32_t lineLimit(const TransformOptions& o) { return o.maxLineLength > 65535 ? 65535 : o.maxLineLength; }

Status runCheck(LineReader& reader, TransformKind kind, const TransformOptions& options, ArenaAlloc& arena, Report& report, Checked& out) {
  report = Report{nullptr, 0, 0, false, false, 0};
  const uint32_t limit = lineLimit(options);
  char* formBuf = arena.array<char>(limit + 3);
  const size_t scratchCap = 4 * static_cast<size_t>(limit) + 4096;
  char* scratch = arena.array<char>(scratchCap);
  IssueCollector issues(arena);
  if (arena.failed || !issues.init()) return Status::OutOfMemory;
  EnabledForm form(reader, formBuf, limit + 3);
  Checker checker(kind, arena, form, issues, scratch, scratchCap);
  AnalysisOptions ao;
  ao.maxLineLength = limit;
  ao.progress = options.progress;
  ao.progressContext = options.progressContext;
  // From the first line, wherever the reader was left (a report's lines are read back with it).
  if (!reader.rewind()) return Status::ReadError;
  const Status s = analyseWith(form, ao, arena, nullptr, &checker, out.analysis);
  if (s == Status::LineTooLong) {
    // The one rule TS doesn't have (15 §4.2): the report blocks on that line.
    const Line& l = form.original();
    const uint32_t bom = l.index == 0 && out.analysis.utf8Bom ? 3 : 0;
    char text[96];
    TextBuilder b(text, sizeof text);
    char num[24];
    const int n = formatCount(limit, num, sizeof num);
    b.put("Line too long: more than ");
    b.put(num, n > 0 ? static_cast<size_t>(n) : 0);
    b.put(" bytes");
    if (!issues.add(0, Severity::Blocking, "B-LINE-TOO-LONG", b.buf, b.len, l.index + 1, l.offset + bom, l.length - bom, false) ||
        !issues.finish(report)) {
      return Status::OutOfMemory;
    }
    report.arenaUsed = arena.used();
    out.hasSetup = false;
    return Status::Ok;
  }
  if (s != Status::Ok) return s;
  if (!checker.finish() || arena.failed || !issues.finish(report)) return Status::OutOfMemory;
  report.arenaUsed = arena.used();
  out.hasSetup = checker.hasSetup;
  out.setup = checker.setup;
  return Status::Ok;
}

// ---- Writing (apply) ----

/** Buffered output. */
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

/** Writes a line with its edits (word edits, and stock edits as they come), noting a change. */
class LineWriter final : public EditSink {
 public:
  LineWriter(Out& out, const char* text, size_t n, const Edit* words, int wordCount)
      : out_(out), text_(text), n_(n), words_(words), wordCount_(wordCount) {}
  bool edit(const Edit& e) override {
    while (next_ < wordCount_ && words_[next_].start < e.start) emit(words_[next_++]);
    emit(e);
    return !out_.failed;
  }
  void finish() {
    while (next_ < wordCount_) emit(words_[next_++]);
    out_.put(text_ + at_, n_ - at_);
  }
  bool changed = false;

 private:
  void emit(const Edit& e) {
    out_.put(text_ + at_, e.start - at_);
    out_.put(e.text, e.length);
    if (e.end - e.start != e.length || std::memcmp(text_ + e.start, e.text, e.length) != 0) changed = true;
    at_ = e.end;
  }
  Out& out_;
  const char* text_;
  size_t n_;
  const Edit* words_;
  int wordCount_;
  int next_ = 0;
  uint32_t at_ = 0;
};

const char* ending(Ending e, size_t& n) {
  switch (e) {
    case Ending::LF: n = 1; return "\n";
    case Ending::CRLF: n = 2; return "\r\n";
    case Ending::CR: n = 1; return "\r";
    default: n = 0; return "";
  }
}

void summarize(TransformKind kind, const Checked& c, uint32_t changed, TransformResult& r) {
  TextBuilder b(r.summary, sizeof r.summary - 1);
  const Analysis& a = c.analysis;
  if (!c.hasSetup) {
    b.put("No XY motion found");
  } else {
    static const char* const kDone[] = {"Rotated 90\xC2\xB0 CW", "Rotated 90\xC2\xB0 CCW", "Flipped horizontally", "Flipped vertically"};
    const bool mirror = kind == TransformKind::FlipH || kind == TransformKind::FlipV;
    char num[32];
    b.put(kDone[static_cast<int>(kind)]);
    b.put(" \xC2\xB7 ");
    int n = formatCount(changed, num, sizeof num);
    b.put(num, n > 0 ? static_cast<size_t>(n) : 0);
    b.put(" of ");
    n = formatCount(a.lineCount, num, sizeof num);
    b.put(num, n > 0 ? static_cast<size_t>(n) : 0);
    b.put(" lines changed");
    if (a.stockCommentCount && !mirror) b.put(" \xC2\xB7 stock rotated");
    if (a.fixedMoveCount) {
      b.put(" \xC2\xB7 ");
      n = formatFixed(a.fixedMoveCount, 0, true, num, sizeof num);
      b.put(num, n > 0 ? static_cast<size_t>(n) : 0);
      b.put(a.fixedMoveCount == 1 ? " probe/zero move kept in place" : " probe/zero moves kept in place");
    }
  }
  r.summary[b.len] = '\0';
  r.summaryLength = static_cast<uint32_t>(b.len);
  r.linesChanged = c.hasSetup ? changed : 0;
  r.lineCount = a.lineCount;
}

}  // namespace

Status check(LineReader& reader, TransformKind kind, const TransformOptions& options, Arena arenaIn, Report& report) {
  ArenaAlloc arena(arenaIn);
  Checked* c = arena.array<Checked>(1);
  if (!c) {
    report = Report{nullptr, 0, 0, false, false, 0};
    return Status::OutOfMemory;
  }
  return runCheck(reader, kind, options, arena, report, *c);
}

Status apply(LineReader& reader, ByteWriter& writer, TransformKind kind, FlipChoice choice, const TransformOptions& options, Arena arenaIn,
             Report& report, TransformResult& result) {
  std::memset(&result, 0, sizeof result);
  report = Report{nullptr, 0, 0, false, false, 0};
  const bool mirror = kind == TransformKind::FlipH || kind == TransformKind::FlipV;
  if (mirror && choice == FlipChoice::FlipAndReverseCuts) return Status::NotSupported;  // reverse cuts: C4
  ArenaAlloc arena(arenaIn);
  Checked* c = arena.array<Checked>(1);
  if (!c) return Status::OutOfMemory;
  const Status s = runCheck(reader, kind, options, arena, report, *c);
  if (s != Status::Ok) return s;
  if (report.blocked) return Status::Blocked;

  const size_t outCap = 4096;
  char* outBuf = arena.array<char>(outCap);
  if (!outBuf) return Status::OutOfMemory;
  if (!reader.rewind()) return Status::ReadError;
  Out out(writer, outBuf, outCap);
  Simulator sim;
  LineState st{};
  const Analysis& a = c->analysis;
  uint32_t fixedAt = 0, stockAt = 0, changed = 0;
  Line l{};
  while (reader.next(l)) {
    const uint32_t i = l.index;
    if (i > 0 && i % kCheckpointLines == 0 && options.progress && !options.progress(options.progressContext, i, l.offset)) {
      return Status::Cancelled;
    }
    const auto* b = reinterpret_cast<const unsigned char*>(l.data);
    const uint32_t bom = i == 0 && l.length >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf ? 3 : 0;
    const uint32_t prefix = offPrefixLength(l.data + bom, l.length - bom);
    const char* text = l.data + bom + prefix;
    const uint32_t n = l.length - bom - prefix;
    out.put(l.data, bom + prefix);  // the BOM, and the `;OFF ` prefix put back (11 §6)
    size_t endLength = 0;
    const char* end = ending(l.ending, endLength);
    if (!c->hasSetup) {
      out.put(text, n);
      out.put(end, endLength);
      continue;
    }
    sim.step(text, n, i, st);
    while (fixedAt < a.fixedMoveCount && a.fixedMoves[fixedAt].line < i) fixedAt++;
    const bool fixed = fixedAt < a.fixedMoveCount && a.fixedMoves[fixedAt].line == i;
    while (stockAt < a.stockCommentCount && a.stockComments[stockAt].line < i) stockAt++;
    const bool stock = stockAt < a.stockCommentCount && a.stockComments[stockAt].line == i;
    const bool words = ((st.letters & XYIJ) != 0 && (st.owner == Owner::None || st.owner == Owner::Motion) && !fixed) ||
                       (mirror && (hasG(st, 20) || hasG(st, 30)));
    if (!words && !stock) {
      out.put(text, n);
      out.put(end, endLength);
      continue;
    }
    Edit edits[5];
    int count = 0;
    // check() reports every overflow (B-OVERFLOW), so these can't fail: an internal error.
    if (words && !wordEdits(text, n, c->setup, st.distance == Distance::G91, edits, count)) return Status::Blocked;
    LineWriter lw(out, text, n, edits, count);
    if (stock && !stockEdits(text, n, i, c->setup, &lw)) return out.failed ? Status::WriteError : Status::Blocked;
    lw.finish();
    if (lw.changed) changed++;
    out.put(end, endLength);
    if (out.failed) return Status::WriteError;
  }
  if (reader.failed()) return Status::ReadError;
  out.flush();
  if (out.failed) return Status::WriteError;
  summarize(kind, *c, changed, result);
  report.arenaUsed = arena.used();
  return Status::Ok;
}

int32_t reportLineText(LineReader& reader, const Issue& issue, char* out, size_t capacity) {
  if (!reader.rewind()) return -1;
  Line l{};
  while (reader.next(l)) {
    if (l.index + 1 != issue.line) continue;
    const uint64_t skip = issue.offset - l.offset;  // a UTF-8 BOM on the first line
    if (issue.offset < l.offset || skip + issue.length > l.length) return -1;
    const size_t n = issue.length < capacity ? issue.length : capacity;
    std::memcpy(out, l.data + skip, n);
    return static_cast<int32_t>(n);
  }
  return -1;
}

}  // namespace gcode
