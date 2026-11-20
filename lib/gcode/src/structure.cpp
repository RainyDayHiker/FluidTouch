// The operation structure (ops/structure.ts findOperationStructure and toolLabels), streaming.
// Each TS regex is emulated exactly; each comment names it.
#include "structure.h"

#include <cstring>

#include "gcode/num.h"

namespace gcode {
namespace {

const char BEGIN_OP[] = "(Begin operation:";
const char FINISH_OP[] = "(Finish operation:";
const char BEGIN_TC[] = "(Begin toolchange)";

bool isBlank(char c) { return c == ' ' || c == '\t'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

struct View {
  const char* p;
  size_t n;
  char at(size_t i) const { return i < n ? p[i] : '\0'; }
  bool startsWith(const char* s) const {
    const size_t k = std::strlen(s);
    return n >= k && std::memcmp(p, s, k) == 0;
  }
};

View trimBlanks(View v) {
  size_t i = 0, j = v.n;
  while (i < j && isBlank(v.p[i])) i++;
  while (j > i && isBlank(v.p[j - 1])) j--;
  return View{v.p + i, j - i};
}

/** stripOff: OFF_RE = /^;OFF(?:\[T(\d+)\])? / */
struct Stripped {
  View text;
  bool off;
  bool hasTag;
  double tag;
};
Stripped stripOff(View v) {
  Stripped s{v, false, false, 0};
  if (!v.startsWith(";OFF")) return s;
  if (v.at(4) == '[' && v.at(5) == 'T' && isDigit(v.at(6))) {
    size_t j = 6;
    while (isDigit(v.at(j))) j++;
    if (v.at(j) == ']' && v.at(j + 1) == ' ') {
      s.off = true;
      s.hasTag = true;
      s.tag = parseDecimal(v.p + 6, j - 6);
      s.text = View{v.p + j + 2, v.n - j - 2};
      return s;
    }
  }
  if (v.at(4) == ' ') {
    s.off = true;
    s.text = View{v.p + 5, v.n - 5};
  }
  return s;
}

/** markerName: "NAME" from "(Begin operation: NAME)". */
View markerName(View t, const char* marker) {
  const size_t k = std::strlen(marker);
  View inner{t.p + k, t.n - k};
  size_t close = inner.n;
  for (size_t i = inner.n; i > 0; i--) {
    if (inner.p[i - 1] == ')') {
      close = i - 1;
      break;
    }
  }
  return trimBlanks(View{inner.p, close});
}

/** COMMENT_ONLY_RE = /^[ \t]*(?:\([^()]*\)[ \t]*)*(?:;.*)?$/ */
bool commentOnly(View t) {
  size_t i = 0;
  while (i < t.n && isBlank(t.p[i])) i++;
  while (i < t.n && t.p[i] == '(') {
    size_t j = i + 1;
    while (j < t.n && t.p[j] != '(' && t.p[j] != ')') j++;
    if (j >= t.n || t.p[j] != ')') break;
    i = j + 1;
    while (i < t.n && isBlank(t.p[i])) i++;
  }
  if (i < t.n && t.p[i] == ';') return true;
  return i == t.n;
}

/** /^[ \t]*\(MSG / */
bool msgLine(View t) {
  size_t i = 0;
  while (i < t.n && isBlank(t.p[i])) i++;
  return t.n - i >= 5 && std::memcmp(t.p + i, "(MSG ", 5) == 0;
}

/** Searches for `prefix` + ([^,()]+) + `suffix`; the group's [start, end) on a match. */
bool groupMatch(View t, const char* prefix, const char* suffix, size_t& start, size_t& end) {
  const size_t pk = std::strlen(prefix), sk = std::strlen(suffix);
  for (size_t s = 0; s + pk <= t.n; s++) {
    if (std::memcmp(t.p + s, prefix, pk) != 0) continue;
    size_t q = s + pk;
    while (q < t.n && t.p[q] != ',' && t.p[q] != '(' && t.p[q] != ')') q++;
    if (q > s + pk && t.n - q >= sk && std::memcmp(t.p + q, suffix, sk) == 0) {
      start = s + pk;
      end = q;
      return true;
    }
  }
  return false;
}

/** M6_RE = /^\(?[ \t]*(?:M0*6[ \t]*T(\d+)|T(\d+)[ \t]*M0*6)(?![0-9.])/i: the tool's digits. */
bool m6Match(View t, size_t& digits, size_t& digitsEnd) {
  size_t i = t.at(0) == '(' ? 1 : 0;
  while (isBlank(t.at(i))) i++;
  const auto lower = [&](size_t k) { return static_cast<char>(t.at(k) | 0x20); };
  // M0*6 [ \t]* T(\d+)
  if (lower(i) == 'm') {
    size_t j = i + 1;
    while (t.at(j) == '0') j++;
    if (t.at(j) == '6') {
      j++;
      while (isBlank(t.at(j))) j++;
      if (lower(j) == 't' && isDigit(t.at(j + 1))) {
        size_t k = j + 1;
        while (isDigit(t.at(k))) k++;
        if (t.at(k) != '.') {
          digits = j + 1;
          digitsEnd = k;
          return true;
        }
      }
    }
  }
  // T(\d+) [ \t]* M0*6
  if (lower(i) == 't' && isDigit(t.at(i + 1))) {
    size_t k = i + 1;
    while (isDigit(t.at(k))) k++;
    size_t j = k;
    while (isBlank(t.at(j))) j++;
    if (lower(j) == 'm') {
      j++;
      while (t.at(j) == '0') j++;
      if (t.at(j) == '6' && !isDigit(t.at(j + 1)) && t.at(j + 1) != '.') {
        digits = i + 1;
        digitsEnd = k;
        return true;
      }
    }
  }
  return false;
}

/**
 * FIXTURE_RE = /^(?:[ \t]*G0*5[4-9](?:\.[1-3])?)+[ \t]*$/i on the line with its comments removed
 * (`t.replace(/\([^()]*\)|;.*$/g, "")`).
 */
bool fixtureLine(View t, char* buf) {
  size_t n = 0;
  for (size_t i = 0; i < t.n; i++) {
    if (t.p[i] == '(') {
      size_t j = i + 1;
      while (j < t.n && t.p[j] != '(' && t.p[j] != ')') j++;
      if (j < t.n && t.p[j] == ')') {
        i = j;
        continue;
      }
    } else if (t.p[i] == ';') {
      break;
    }
    buf[n++] = t.p[i];
  }
  const View s{buf, n};
  size_t i = 0;
  int groups = 0;
  for (;;) {
    size_t j = i;
    while (isBlank(s.at(j)) && j < s.n) j++;
    if ((s.at(j) | 0x20) != 'g' || j >= s.n) break;
    j++;
    while (s.at(j) == '0') j++;
    if (s.at(j) != '5' || s.at(j + 1) < '4' || s.at(j + 1) > '9') break;
    j += 2;
    if (s.at(j) == '.' && s.at(j + 1) >= '1' && s.at(j + 1) <= '3') j += 2;
    groups++;
    i = j;
  }
  if (groups == 0) return false;
  while (i < s.n && isBlank(s.p[i])) i++;
  return i == s.n;
}

bool same(Str a, View b) { return a.length == b.n && std::memcmp(a.data, b.p, b.n) == 0; }

}  // namespace

void StructureFinder::invalid(const char* a, Str name, const char* b, uint32_t index) {
  if (done_) return;
  TextBuilder t(scratch_, scratchSize_);
  t.put(a);
  t.put(name);
  t.put(b);
  reason_ = arena_.copy(t.buf, t.len);
  reasonLine_ = index + 1;
  done_ = true;
}

void StructureFinder::invalid2(const char* a, Str n1, const char* b, Str n2, const char* c, uint32_t index) {
  if (done_) return;
  TextBuilder t(scratch_, scratchSize_);
  t.put(a);
  t.put(n1);
  t.put(b);
  t.put(n2);
  t.put(c);
  reason_ = arena_.copy(t.buf, t.len);
  reasonLine_ = index + 1;
  done_ = true;
}

void StructureFinder::step(const char* text, size_t length, uint32_t i) {
  if (done_) return;
  const View raw{text, length};
  const Stripped s = stripOff(raw);
  const View t = trimBlanks(s.text);
  const Str none{"", 0};

  if (t.startsWith(BEGIN_OP)) {
    if (open_) return invalid("\"", cur_.name, "\" has no Finish before the next operation begins", i);
    uint32_t start = i;
    if (afterPrev_ >= 0) {
      // Everything between two blocks is the next one's leading lines: comments only.
      if (firstNonComment_ >= 0) return invalid("G-code between two operations, outside any of them", none, "", static_cast<uint32_t>(firstNonComment_));
      start = static_cast<uint32_t>(afterPrev_);
    } else {
      // Before the first block: only its own (MSG …) lines lead it; the rest is the preamble.
      start = i - msgRun_;
      firstBlockSeen_ = true;
      firstStart_ = start;
      preambleProbes_ = beforeRun_;
    }
    const View name = markerName(t, BEGIN_OP);
    cur_ = Block{};
    cur_.kind = 0;
    cur_.name = arena_.copy(name.p, name.n);
    cur_.start = start;
    cur_.begin = i;
    cur_.off = s.off;
    cur_.hasTaggedTool = s.hasTag;
    cur_.taggedTool = s.tag;
    cur_.beginToolchange = -1;
    cur_.m6 = -1;
    nonComment_ = fixtureLines_ = 0;
    open_ = true;
    return;
  }

  if (!firstBlockSeen_) {
    // The preamble's (MSG …) run and probe lines, for the first block's start and the probe.
    const bool msg = msgLine(s.text);
    size_t ls = 0, le = 0;
    const bool hit = groupMatch(raw, "(MSG Insert ", ", attach probe)", ls, le);
    ProbeHit h{};
    if (hit) {
      const View label = trimBlanks(View{text + ls, le - ls});
      h = ProbeHit{1, i, arena_.copy(label.p, label.n), static_cast<uint32_t>(ls), static_cast<uint32_t>(le)};
    }
    if (!msg) {
      if (inRun_.count) {
        const uint32_t c = beforeRun_.count + inRun_.count;
        beforeRun_ = inRun_;
        beforeRun_.count = c;
      }
      inRun_.count = 0;
      msgRun_ = 0;
      if (hit) {
        const uint32_t c = beforeRun_.count + 1;
        beforeRun_ = h;
        beforeRun_.count = c;
      }
    } else {
      msgRun_++;
      if (hit) {
        const uint32_t c = inRun_.count + 1;
        inRun_ = h;
        inRun_.count = c;
      }
    }
  }

  if (!open_) {
    if (t.startsWith(FINISH_OP)) return invalid("Finish operation without its Begin", none, "", i);
    if (afterPrev_ >= 0 && firstNonComment_ < 0 && !commentOnly(s.text)) firstNonComment_ = i;
    return;
  }
  if (s.off != cur_.off) return invalid("\"", cur_.name, "\" is only partly commented out (;OFF)", i);
  if (t.startsWith(FINISH_OP)) {
    const View name = markerName(t, FINISH_OP);
    if (!same(cur_.name, name)) return invalid2("\"", cur_.name, "\" ends with a Finish for \"", arena_.copy(name.p, name.n), "\"", i);
    cur_.end = i + 1;
    if (cur_.m6 >= 0 || cur_.beginToolchange >= 0) {
      if (!cur_.hasTool) return invalid("Tool change \"", cur_.name, "\" doesn't say which tool (no M6 T\xE2\x80\xA6)", cur_.begin);
      cur_.kind = 1;
    } else if (nonComment_ > 0 && fixtureLines_ == nonComment_) {
      cur_.kind = 2;
    }
    if (!blocks_.push(arena_, cur_)) {
      done_ = true;
      return;
    }
    open_ = false;
    afterPrev_ = i + 1;
    firstNonComment_ = -1;
    return;
  }
  if (t.startsWith(BEGIN_TC)) {
    cur_.beginToolchange = static_cast<int32_t>(i);
    return;
  }
  // M6 lines start with "(", "M", or "T": most lines (G moves) skip the regex.
  const char c0 = static_cast<char>((t.n ? t.p[0] : 0) | 0x20);
  size_t ds = 0, de = 0;
  if (cur_.m6 < 0 && (c0 == '(' || c0 == 'm' || c0 == 't') && m6Match(t, ds, de)) {
    cur_.m6 = static_cast<int32_t>(i);
    cur_.hasTool = true;
    cur_.tool = parseDecimal(t.p + ds, de - ds);
    return;
  }
  if (cur_.beginToolchange >= 0 && cur_.m6 < 0 && !cur_.hasLabel) {
    size_t ls = 0, le = 0;
    if (groupMatch(t, "(MSG Tool change: ", ",", ls, le)) {
      const View label = trimBlanks(View{t.p + ls, le - ls});
      cur_.label = arena_.copy(label.p, label.n);
      cur_.hasLabel = true;
    }
  }
  // Only a block that could still be a fixture needs its lines classified.
  if (nonComment_ == fixtureLines_ && !commentOnly(t)) {
    nonComment_++;
    if (fixtureLine(t, scratch_)) fixtureLines_++;
  }
}

void StructureFinder::finish(Structure& out) {
  std::memset(&out, 0, sizeof out);
  out.reason = Str{"", 0};
  out.probeLabel = Str{"", 0};
  const Str none{"", 0};
  const auto fail = [&]() {
    out.kind = StructureKind::Invalid;
    out.reason = reason_;
    out.reasonLine = reasonLine_;
  };
  if (done_) return fail();
  if (open_) {
    invalid("\"", cur_.name, "\" has no Finish", cur_.begin);
    return fail();
  }
  bool anyOp = false;
  for (uint32_t k = 0; k < blocks_.size; k++) anyOp = anyOp || blocks_.data[k].kind == 0;
  if (!anyOp) {
    out.kind = StructureKind::None;
    out.reason = Str{"No operations found", 19};
    out.reasonLine = 0;
    return;
  }
  for (uint32_t k = 0; k < blocks_.size; k++) {
    const Block& b = blocks_.data[k];
    if (b.kind == 1 && b.m6 < 0) {
      invalid("Tool change \"", b.name, "\" doesn't say which tool (no M6 T\xE2\x80\xA6)", b.begin);
      return fail();
    }
  }

  // Units (11 §1): fixtures and tool changes attach to the operation after them.
  Vec<Unit> units;
  Vec<ToolChangeBlock> tcs;
  Vec<LineRange> fixtures;
  Vec<int32_t> spares;
  int fixturesOff = -1;
  int32_t liveTc = -1;
  bool hasTool = false;
  double tool = 0;
  for (uint32_t k = 0; k < blocks_.size; k++) {
    const Block& b = blocks_.data[k];
    if (b.kind == 2) {
      if (fixturesOff >= 0 && fixturesOff != static_cast<int>(b.off)) {
        invalid("Fixtures before one operation are partly commented out", none, "", b.begin);
        return fail();
      }
      fixtures.push(arena_, LineRange{b.start, b.end});
      fixturesOff = b.off;
      continue;
    }
    if (b.kind == 1) {
      ToolChangeBlock tc{};
      tc.tool = b.tool;
      tc.name = b.name;
      tc.range = LineRange{b.start, b.end};
      tc.off = b.off;
      tc.beginToolchange = b.beginToolchange;
      tc.m6 = b.m6;
      tc.steps = b.beginToolchange >= 0 && b.beginToolchange < b.m6
                     ? LineRange{static_cast<uint32_t>(b.beginToolchange + 1), static_cast<uint32_t>(b.m6)}
                     : LineRange{static_cast<uint32_t>(b.m6), static_cast<uint32_t>(b.m6)};
      tc.label = b.hasLabel ? b.label : none;
      tc.hasLabel = b.hasLabel;
      tcs.push(arena_, tc);
      const int32_t idx = static_cast<int32_t>(tcs.size - 1);
      if (tc.off) {
        spares.push(arena_, idx);
      } else {
        if (liveTc >= 0) {
          invalid("Tool change \"", tcs.data[liveTc].name, "\" isn't followed by an operation", tcs.data[liveTc].range.start);
          return fail();
        }
        liveTc = idx;
        hasTool = true;
        tool = tc.tool;
      }
      continue;
    }
    if (fixturesOff >= 0 && fixturesOff != static_cast<int>(b.off)) {
      invalid2("The fixture before \"", b.name, b.off ? "\" is on but the operation isn't" : "\" is off but the operation isn't", none, "", fixtures.data[0].start);
      return fail();
    }
    if (b.off && liveTc >= 0) {
      invalid("Tool change \"", tcs.data[liveTc].name, "\" is followed by an operation that's off", tcs.data[liveTc].range.start);
      return fail();
    }
    Unit u{};
    u.index = units.size;
    u.name = b.name;
    u.hasTool = b.off ? (b.hasTaggedTool || hasTool) : hasTool;
    u.tool = b.off ? (b.hasTaggedTool ? b.taggedTool : tool) : tool;
    u.enabled = !b.off;
    u.fixtures = fixtures.data;
    u.fixtureCount = fixtures.size;
    u.body = LineRange{b.start, b.end};
    u.beginLine = b.begin;
    u.toolChange = liveTc;
    u.spares = spares.data;
    u.spareCount = spares.size;
    units.push(arena_, u);
    fixtures = Vec<LineRange>{};
    fixturesOff = -1;
    liveTc = -1;
    spares = Vec<int32_t>{};
  }
  if (liveTc >= 0) {
    invalid("Tool change \"", tcs.data[liveTc].name, "\" isn't followed by an operation", tcs.data[liveTc].range.start);
    return fail();
  }
  if (fixtures.size) {
    invalid("A fixture after the last operation", none, "", fixtures.data[0].start);
    return fail();
  }
  if (spares.size) {
    invalid("A spare tool change after the last operation", none, "", tcs.data[spares.data[0]].range.start);
    return fail();
  }

  out.kind = StructureKind::Ok;
  out.preambleEnd = blocks_.data[0].start;
  out.postambleStart = blocks_.back().end;
  out.units = units.data;
  out.unitCount = units.size;
  out.toolChanges = tcs.data;
  out.toolChangeCount = tcs.size;
  if (preambleProbes_.count == 1) {
    out.hasProbe = true;
    out.probeLine = preambleProbes_.line;
    out.probeLabel = preambleProbes_.label;
    out.probeLabelStart = preambleProbes_.labelStart;
    out.probeLabelEnd = preambleProbes_.labelEnd;
  }

  // toolLabels (11 §7): label from a tool change's MSG step, then the probe label for the first
  // live tool, then "T<tool> <name without TCn:>".
  Vec<double> labelTools;
  Vec<Str> labels;
  const auto has = [&](double t) {
    for (uint32_t k = 0; k < labelTools.size; k++) {
      if (labelTools.data[k] == t) return true;
    }
    return false;
  };
  for (uint32_t k = 0; k < tcs.size; k++) {
    const ToolChangeBlock& tc = tcs.data[k];
    if (tc.hasLabel && !has(tc.tool)) {
      labelTools.push(arena_, tc.tool);
      labels.push(arena_, tc.label);
    }
  }
  const ToolChangeBlock* first = nullptr;
  for (uint32_t k = 0; k < tcs.size && !first; k++) {
    if (!tcs.data[k].off) first = &tcs.data[k];
  }
  if (out.hasProbe && first && !has(first->tool)) {
    labelTools.push(arena_, first->tool);
    labels.push(arena_, out.probeLabel);
  }
  for (uint32_t k = 0; k < tcs.size; k++) {
    const ToolChangeBlock& tc = tcs.data[k];
    if (has(tc.tool)) continue;
    // tc.name.replace(/^TC\d*:[ \t]*/, "")
    View after{tc.name.data, tc.name.length};
    if (after.startsWith("TC")) {
      size_t j = 2;
      while (isDigit(after.at(j))) j++;
      if (after.at(j) == ':') {
        j++;
        while (j < after.n && isBlank(after.p[j])) j++;
        after = View{after.p + j, after.n - j};
      }
    }
    TextBuilder t(scratch_, scratchSize_);
    char num[48];
    const int len = formatFixed(tc.tool, 0, true, num, sizeof num);
    t.put("T");
    t.put(num, len > 0 ? static_cast<size_t>(len) : 0);
    t.put(" ");
    t.put(after.p, after.n);
    labelTools.push(arena_, tc.tool);
    labels.push(arena_, arena_.copy(t.buf, t.len));
  }
  out.labelTools = labelTools.data;
  out.labels = labels.data;
  out.labelCount = labelTools.size;
}

uint32_t offPrefixLength(const char* text, size_t length) {
  const View v{text, length};
  const Stripped s = stripOff(v);
  return s.off ? static_cast<uint32_t>(s.text.p - text) : 0;
}

Str trimmedBlanks(const char* text, size_t length) {
  const View t = trimBlanks(View{text, length});
  return Str{t.p, static_cast<uint32_t>(t.n)};
}

LineMarker lineMarker(const char* text, size_t length, Str& name) {
  const View t = trimBlanks(View{text, length});
  if (t.startsWith(BEGIN_OP)) {
    const View n = markerName(t, BEGIN_OP);
    name = Str{n.p, static_cast<uint32_t>(n.n)};
    return LineMarker::BeginOperation;
  }
  if (t.startsWith(FINISH_OP)) return LineMarker::FinishOperation;
  size_t d = 0, e = 0;
  if (t.startsWith(BEGIN_TC) || m6Match(t, d, e)) return LineMarker::ToolChange;
  return LineMarker::None;
}

}  // namespace gcode
