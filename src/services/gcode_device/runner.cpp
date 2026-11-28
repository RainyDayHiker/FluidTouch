// The device test suite's runner (specs/15 §9.4); the data format is in data/FORMAT.md of the
// suite (written by tools/package-core-cpp.mjs). Platform-free: only the library and <cstdio>,
// <cstdlib>, <cstring>, <cmath>, <new>. No static buffers: the runner's scratch comes from the
// front of the caller's arena (Scratch below), so a firmware that keeps the runner costs no RAM
// until the suite runs.
#include "runner.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "gcode/analysis.h"
#include "gcode/num.h"
#include "gcode/outline.h"
#include "gcode/transform.h"
#include "gcode/surfacing.h"
#include "gcode/version.h"
#include "sha256.h"

namespace gcode_device {
namespace {

using namespace gcode;
using gcode_test::Sha256;

// ---- Text helpers ----------------------------------------------------------------------------

/**
 * A line copied out of the reader (its data is valid only until the next read), split on tabs.
 * A missing field reads as the empty string at the line's end, inside `buf` (not a literal, so
 * a compiler can't take `field + k` past a one-byte string).
 */
struct Fields {
  char buf[4096];
  const char* f[64];
  const char* end = buf;
  int n = 0;
  bool load(const Line& line) {
    const size_t len = line.length < sizeof(buf) - 1 ? line.length : sizeof(buf) - 1;
    std::memcpy(buf, line.data, len);
    buf[len] = '\0';
    end = buf + len;
    n = 0;
    f[n++] = buf;
    for (size_t i = 0; i < len && n < 64; i++) {
      if (buf[i] == '\t') {
        buf[i] = '\0';
        f[n++] = buf + i + 1;
      }
    }
    return true;
  }
  const char* operator[](int i) const { return i < n ? f[i] : end; }
  bool is(int i, const char* s) const { return std::strcmp((*this)[i], s) == 0; }
};

/**
 * The runner's working buffers, placed at the front of the caller's arena by run(); the rest of
 * the arena goes to the library. `manifest` holds the manifest line for the whole case (its
 * fields name the case's files), `group` the case's data lines, `state` the expected per-line
 * states read inside the analysis hook. `text` is the structure reason's hex in the analyse
 * group and the write block in the perf group, which never run at once.
 */
struct Scratch {
  Fields manifest, group, state;
  char text[4096];
};

double fromHex(const char* h) {
  const unsigned long long u = std::strtoull(h, nullptr, 16);
  double x;
  std::memcpy(&x, &u, sizeof x);
  return x;
}

bool sameDouble(double a, double b) {
  if (a != a && b != b) return true;
  return std::memcmp(&a, &b, sizeof a) == 0;
}

/** An expected fixed-point integer ("null" or digits) against a double (NaN for null). */
bool sameFixed(const char* want, double v) {
  if (std::strcmp(want, "null") == 0) return std::isnan(v);
  return !std::isnan(v) && std::strtod(want, nullptr) == v;
}

/** An expected double (hex bits or "null") against a double. */
bool sameHex(const char* want, double v) {
  if (std::strcmp(want, "null") == 0) return std::isnan(v);
  return sameDouble(fromHex(want), v);
}

bool sameText(const char* want, const char* got, size_t gotLen) {
  return std::strlen(want) == gotLen && std::memcmp(want, got, gotLen) == 0;
}

/** Bytes as lowercase hex, into `out` (terminated). */
void toHex(const char* p, size_t n, char* out, size_t cap) {
  static const char d[] = "0123456789abcdef";
  size_t k = 0;
  for (size_t i = 0; i < n && k + 2 < cap; i++) {
    const unsigned char c = static_cast<unsigned char>(p[i]);
    out[k++] = d[c >> 4];
    out[k++] = d[c & 15];
  }
  out[k] = '\0';
}

const char* endingName(Ending e) {
  return e == Ending::LF ? "LF" : e == Ending::CRLF ? "CRLF" : e == Ending::CR ? "CR" : "None";
}

const char* statusName(Status s) {
  static const char* const k[] = {"Ok",      "Refused",      "LineTooLong", "ReadError",  "OutOfMemory",      "Cancelled",
                                  "Blocked", "NotSupported", "WriteError",  "FileChanged", "InvalidParameters"};
  const int i = static_cast<int>(s);
  return i >= 0 && i < 11 ? k[i] : "?";
}

/** Whether `hex` (lowercase) is exactly the hex of bytes[0, n). */
bool sameHexBytes(const char* hex, const char* bytes, size_t n) {
  static const char d[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = static_cast<unsigned char>(bytes[i]);
    if (hex[2 * i] != d[c >> 4] || hex[2 * i + 1] != d[c & 15]) return false;
  }
  return hex[2 * n] == '\0';
}

/** A transform's output, hashed and counted, not kept. */
class HashWriter final : public ByteWriter {
 public:
  Sha256 sha;
  uint64_t length = 0;
  bool write(const char* data, size_t n) override {
    sha.update(data, n);
    length += n;
    return true;
  }
};

// ---- Reporting -------------------------------------------------------------------------------

struct Report {
  explicit Report(Platform& platform) : p(platform) {}
  Platform& p;
  Totals totals{0, 0, 0};
  char line[512] = {};

  void result(const char* verdict, const char* id, const char* detail) {
    std::snprintf(line, sizeof line, "%s %s%s%s", verdict, id, detail && *detail ? " " : "", detail ? detail : "");
    p.print(line);
    if (verdict[0] == 'P' || verdict[0] == 'I') totals.passed++;
    else if (verdict[0] == 'F') totals.failed++;
    else totals.skipped++;
  }
};

/** Collects the first mismatch of a case. */
struct Mismatch {
  char text[400];
  bool any = false;
  void set(const char* fmt, const char* a, const char* b = "") {
    if (any) return;
    any = true;
    std::snprintf(text, sizeof text, fmt, a, b);
  }
};

bool wanted(const char* groups, const char* group) {
  if (!groups) return true;
  const size_t n = std::strlen(group);
  for (const char* p = groups; *p;) {
    const char* comma = std::strchr(p, ',');
    const size_t len = comma ? static_cast<size_t>(comma - p) : std::strlen(p);
    if (len == n && std::strncmp(p, group, n) == 0) return true;
    if (!comma) break;
    p = comma + 1;
  }
  return false;
}

// ---- float and parse groups -------------------------------------------------------------------

void floatGroup(Platform& platform, Report& r, Fields& f, const char* id, const char* file) {
  LineReader* in = platform.open(file);
  if (!in) return r.result("FAIL", id, "can't open the vectors");
  Line line{};
  uint32_t count = 0;
  Mismatch m;
  while (in->next(line)) {
    f.load(line);
    if (f.n < 3) continue;
    count++;
    double got = 0, want = 0;
    if (f.n == 3) {
      const double x = fromHex(f[1]);
      want = fromHex(f[2]);
      if (f.is(0, "sin")) got = fmath::sin(x);
      else if (f.is(0, "cos")) got = fmath::cos(x);
      else if (f.is(0, "acos")) got = fmath::acos(x);
      else if (f.is(0, "round")) got = jsRound(x);
      else if (f.is(0, "roundHalfAwayFromZero")) got = roundHalfAwayFromZero(x);
      else if (f.is(0, "floor")) got = std::floor(x);
      else if (f.is(0, "sqrt")) got = std::sqrt(x);
    } else {
      const double x = fromHex(f[1]), y = fromHex(f[2]);
      want = fromHex(f[3]);
      if (f.is(0, "atan2")) got = fmath::atan2(x, y);
      else if (f.is(0, "hypot")) got = fmath::hypot(x, y);
      else if (f.is(0, "fmod")) got = std::fmod(x, y);
    }
    if (!sameDouble(got, want)) m.set("%s(%s)", f[0], f[1]);
  }
  platform.close(in);
  char detail[480];
  std::snprintf(detail, sizeof detail, "%u calls%s%s", static_cast<unsigned>(count), m.any ? ", first mismatch: " : "", m.any ? m.text : "");
  r.result(m.any ? "FAIL" : "PASS", id, detail);
}

void parseGroup(Platform& platform, Report& r, Fields& f, const char* id, const char* file) {
  LineReader* in = platform.open(file);
  if (!in) return r.result("FAIL", id, "can't open the vectors");
  Line line{};
  uint32_t count = 0;
  Mismatch m;
  char out[128];
  while (in->next(line)) {
    f.load(line);
    if (f.n < 3) continue;
    count++;
    bool ok = true;
    const auto text = [&](int len, const char* want) {
      return len < 0 ? std::strcmp(want, "error") == 0 : sameText(want, out, static_cast<size_t>(len));
    };
    if (f.is(0, "number")) {
      ok = sameDouble(parseDecimal(f[1], std::strlen(f[1])), fromHex(f[2]));
    } else if (f.is(0, "fixed")) {
      int64_t v = 0;
      const bool parsed = parseFixed(f[1], std::strlen(f[1]), std::atoi(f[2]), &v);
      ok = parsed ? std::strtod(f[3], nullptr) == static_cast<double>(v) && !f.is(3, "null") : f.is(3, "null");
    } else if (f.is(0, "formatFixed")) {
      ok = text(formatFixed(std::strtod(f[1], nullptr), std::atoi(f[2]), f.is(3, "1"), out, sizeof out), f[4]);
    } else if (f.is(0, "decimal")) {
      ok = text(formatDecimal(fromHex(f[1]), std::atoi(f[2]), f.is(3, "1"), out, sizeof out), f[4]);
    } else if (f.is(0, "significant")) {
      ok = text(formatSignificant(fromHex(f[1]), std::atoi(f[2]), out, sizeof out), f[3]);
    } else if (f.is(0, "count")) {
      ok = text(formatCount(std::strtod(f[1], nullptr), out, sizeof out), f[2]);
    } else if (f.is(0, "toFixed")) {
      ok = sameDouble(toFixed(fromHex(f[1]), std::atoi(f[2])), std::strtod(f[3], nullptr));
    }
    if (!ok) m.set("%s %s", f[0], f[1]);
  }
  platform.close(in);
  char detail[480];
  std::snprintf(detail, sizeof detail, "%u calls%s%s", static_cast<unsigned>(count), m.any ? ", first mismatch: " : "", m.any ? m.text : "");
  r.result(m.any ? "FAIL" : "PASS", id, detail);
}

// ---- self group: checks that need no data ----------------------------------------------------

void selfGroup(Report& r, Arena arena) {
  Analysis a;
  const auto run = [&](const char* text, size_t n, AnalysisOptions o = AnalysisOptions{}) {
    MemoryLineReader reader(text, n);
    return analyse(reader, o, arena, nullptr, a);
  };
  {
    static const char kUtf16[] = "\xff\xfeG\0";
    const bool ok = run(kUtf16, 4) == Status::Refused;
    r.result(ok ? "PASS" : "FAIL", "self.utf16-refused", "");
  }
  {
    static const char kNul[] = "G1 X1\nG1\0Y2\n";
    r.result(run(kNul, 12) == Status::Refused ? "PASS" : "FAIL", "self.nul-refused", "");
  }
  {
    static const char kLong[] = "G0 X1\nG1 X12345678\n";
    AnalysisOptions o;
    o.maxLineLength = 10;
    r.result(run(kLong, sizeof kLong - 1, o) == Status::LineTooLong && a.longLine == 1 ? "PASS" : "FAIL", "self.line-too-long", "");
  }
  {
    static const char kJob[] = "(Stock: XMIN=0 XMAX=10 YMIN=0 YMAX=10 ZMIN=-1 ZMAX=0)\nG21 G90\nG0 X0 Y0\nG2 X10 Y0 I5 J0\n";
    MemoryLineReader reader(kJob, sizeof kJob - 1);
    Arena tiny{arena.base, 256};
    r.result(analyse(reader, AnalysisOptions{}, tiny, nullptr, a) == Status::OutOfMemory ? "PASS" : "FAIL", "self.out-of-memory", "");
  }
  {
    // 25,000 lines from a generator reader: the second checkpoint cancels.
    class Many final : public LineReader {
     public:
      uint32_t i = 0;
      bool next(Line& out) override {
        if (i >= 25000) return false;
        out = Line{"G1 X1 Y1", 8, Ending::LF, i, static_cast<uint64_t>(i) * 9};
        i++;
        return true;
      }
      bool rewind() override {
        i = 0;
        return true;
      }
      bool failed() const override { return false; }
    } many;
    AnalysisOptions o;
    static uint32_t calls;
    calls = 0;
    o.progress = [](void*, uint32_t, uint64_t) { return ++calls < 2; };
    r.result(analyse(many, o, arena, nullptr, a) == Status::Cancelled && calls == 2 ? "PASS" : "FAIL", "self.cancel", "");
  }
  {
    // Bits that depend on software doubles and newlib's strtod on the device.
    const bool ok = sameDouble(parseDecimal("0.30000000000000004", 19), 0.30000000000000004) &&
                    sameDouble(parseDecimal("123456789012345678", 18), 123456789012345678.0) &&
                    sameDouble(fmath::sin(1.0), 0.8414709848078965) && sameDouble(fmath::atan2(1.0, 1.0), 0.7853981633974483);
    r.result(ok ? "PASS" : "FAIL", "self.doubles", "");
  }
}

// ---- analyse group ---------------------------------------------------------------------------

/** Compares each line's state with the next S line of expected.tsv. */
class StateCheck final : public AnalysisSink {
 public:
  StateCheck(LineReader* expected, Fields& fields, Mismatch& m) : expected_(expected), f_(fields), m_(m) {}
  uint32_t lines = 0;

  void line(const Line& line, const Token*, uint16_t, const LineState& st) override {
    Line l{};
    if (!expected_->next(l)) {
      m_.set("expected.tsv ended at line %s%s", "", "");
      return;
    }
    f_.load(l);
    if (!f_.is(0, "S")) {
      m_.set("expected an S line, got %s%s", f_[0], "");
      return;
    }
    lines++;
    char where[32];
    std::snprintf(where, sizeof where, "%u", static_cast<unsigned>(line.index));
    const auto want = [&](bool ok, const char* field) {
      if (!ok) m_.set("line %s: %s", where, field);
    };
    want(std::strtod(f_[1], nullptr) == line.index, "i");
    want(std::strtod(f_[2], nullptr) == static_cast<double>(line.offset), "offset");
    want(std::strtod(f_[3], nullptr) == line.length, "length");
    want(f_.is(4, endingName(line.ending)), "ending");
    want(std::strtod(f_[5], nullptr) == st.parseFlags, "parseFlags");
    want(f_.is(6, motionName(st.motion)), "motion");
    want(f_.is(7, planeName(st.plane)), "plane");
    want(f_.is(8, distanceName(st.distance)), "distance");
    want(f_.is(9, arcDistanceName(st.arcDistance)), "arcDistance");
    want(f_.is(10, unitsModeName(st.units)), "units");
    want(f_.is(11, wcsName(st.wcs)), "wcs");
    want(f_.is(12, feedModeName(st.feedMode)), "feedMode");
    want(sameFixed(f_[13], st.before.x) && sameFixed(f_[14], st.before.y) && sameHex(f_[15], st.before.z), "before");
    want(sameFixed(f_[16], st.after.x) && sameFixed(f_[17], st.after.y) && sameHex(f_[18], st.after.z), "after");
    want(f_.is(19, st.owner == Owner::None ? "-" : ownerName(st.owner)), "owner");
    want(std::strtod(f_[20], nullptr) == st.flags, "flags");
    want(arcMatches(f_[21], st.arc), "arc");
  }

 private:
  static bool arcMatches(const char* want, const ArcInfo& arc) {
    if (std::strcmp(want, "-") == 0) return arc.state == ArcState::None;
    if (want[0] == 'x') return arc.state == ArcState::Invalid && sameText(want + 2, arc.reason, arc.reasonLength);
    if (want[0] != 'o' || arc.state != ArcState::Ok) return false;
    // o:cx,cy,r,startAngle,sweep[;x,y]...
    const ArcGeometry& g = arc.geometry;
    const double vals[5] = {g.cx, g.cy, g.r, g.startAngle, g.sweep};
    const char* p = want + 2;
    for (int k = 0; k < 5; k++) {
      if (!sameDouble(fromHex(p), vals[k])) return false;
      p += 16;
      if (k < 4 && *p++ != ',') return false;
    }
    int e = 0;
    while (*p == ';') {
      if (e >= g.extremeCount) return false;
      p++;
      if (!sameDouble(fromHex(p), g.extremeX[e])) return false;
      p += 17;
      if (!sameDouble(fromHex(p), g.extremeY[e])) return false;
      p += 16;
      e++;
    }
    return e == g.extremeCount && *p == '\0';
  }

  LineReader* expected_;
  Fields& f_;
  Mismatch& m_;
};

bool extentsMatch(const Fields& f, const Extents& e, bool hex) {
  if (f.is(2, "null")) return !e.present;
  if (!e.present) return false;
  const double v[4] = {e.minX, e.maxX, e.minY, e.maxY};
  for (int k = 0; k < 4; k++) {
    if (hex ? !sameHex(f[2 + k], v[k]) : !sameFixed(f[2 + k], v[k])) return false;
  }
  return true;
}

void analyseGroup(Platform& platform, Report& r, Scratch& sc, Arena arena, const char* id, const char* inputPath, const char* expectedPath) {
  LineReader* input = platform.open(inputPath);
  LineReader* expected = platform.open(expectedPath);
  if (!input || !expected) {
    if (input) platform.close(input);
    if (expected) platform.close(expected);
    return r.result("FAIL", id, "can't open the case's files");
  }
  Mismatch m;
  Line l{};
  Fields& f = sc.group;
  expected->next(l);
  f.load(l);  // H	states	0|1
  const bool states = f.is(0, "H") && f.is(2, "1");
  StateCheck check(expected, sc.state, m);
  Analysis a;
  const Status s = analyse(*input, AnalysisOptions{}, arena, states ? &check : nullptr, a);
  if (s == Status::OutOfMemory) {
    platform.close(input);
    platform.close(expected);
    return r.result("SKIP", id, "needs more memory than the arena");
  }
  while (expected->next(l)) {
    f.load(l);
    if (!f.is(0, "A")) continue;
    if (f.is(1, "refused")) {
      if (s != Status::Refused || std::strcmp(f[2], a.refusedMessage) != 0) m.set("refusal: %s%s", f[2], "");
    } else if (s != Status::Ok) {
      m.set("status %s%s", s == Status::Refused ? "refused" : "not ok", "");
      break;
    } else if (f.is(1, "precision")) {
      if (std::strtod(f[2], nullptr) != a.precision) m.set("precision: want %s%s", f[2], "");
    } else if (f.is(1, "lineCount")) {
      if (std::strtod(f[2], nullptr) != a.lineCount) m.set("lineCount: want %s%s", f[2], "");
    } else if (f.is(1, "bom")) {
      if (f.is(2, "1") != a.utf8Bom) m.set("bom%s%s", "", "");
    } else if (f.is(1, "units")) {
      if (!f.is(2, a.units == Units::Mm ? "mm" : "in") || f.is(3, "1") != a.unitsAssumed || f.is(4, "1") != a.unitsMixed) m.set("units%s%s", "", "");
    } else if (f.is(1, "extents")) {
      if (!extentsMatch(f, a.extents, false)) m.set("extents%s%s", "", "");
    } else if (f.is(1, "display")) {
      if (!extentsMatch(f, a.displayExtents, true)) m.set("displayExtents%s%s", "", "");
    } else if (f.is(1, "job")) {
      if (!extentsMatch(f, a.jobExtents, false)) m.set("jobExtents%s%s", "", "");
    } else if (f.is(1, "stockBox")) {
      if (!extentsMatch(f, a.stockBox, false)) m.set("stockBox%s%s", "", "");
    } else if (f.is(1, "fixed")) {
      bool ok = std::strtod(f[2], nullptr) == a.fixedMoveCount;
      for (uint32_t k = 0; ok && k < a.fixedMoveCount; k++) {
        char item[32];
        std::snprintf(item, sizeof item, "%u:%s", static_cast<unsigned>(a.fixedMoves[k].line), a.fixedMoves[k].kind == FixedMoveKind::Probe ? "probe" : "zero");
        ok = f.is(3 + static_cast<int>(k), item);
      }
      if (!ok) m.set("fixed moves%s%s", "", "");
    } else if (f.is(1, "firstXY")) {
      if (std::strtod(f[2], nullptr) != a.firstXYMotion) m.set("firstXYMotion%s%s", "", "");
    } else if (f.is(1, "stock")) {
      if (std::strtod(f[2], nullptr) != a.stockCommentCount) m.set("stock comments%s%s", "", "");
    } else if (f.is(1, "structure")) {
      const Structure& st = a.structure;
      const char* kind = st.kind == StructureKind::Ok ? "ok" : st.kind == StructureKind::None ? "none" : "invalid";
      toHex(st.reason.data, st.reason.length, sc.text, sizeof sc.text);
      bool ok = f.is(2, kind);
      if (ok && st.kind != StructureKind::Ok) ok = f.is(3, sc.text) && std::strtod(f[4], nullptr) == st.reasonLine;
      if (ok && st.kind == StructureKind::Ok) {
        ok = std::strtod(f[5], nullptr) == st.preambleEnd && std::strtod(f[6], nullptr) == st.postambleStart &&
             std::strtod(f[7], nullptr) == st.unitCount && std::strtod(f[8], nullptr) == st.toolChangeCount &&
             std::strtod(f[9], nullptr) == (st.hasProbe ? static_cast<double>(st.probeLine) : -1.0);
      }
      if (!ok) m.set("structure%s%s", "", "");
    } else if (f.is(1, "flags")) {
      if (std::strtod(f[2], nullptr) != a.parseFlags || std::strtod(f[3], nullptr) != a.simFlags) m.set("flags%s%s", "", "");
    }
  }
  platform.close(input);
  platform.close(expected);
  char detail[480];
  std::snprintf(detail, sizeof detail, "%u lines%s%s", static_cast<unsigned>(a.lineCount), m.any ? ", first mismatch: " : "", m.any ? m.text : "");
  r.result(m.any ? "FAIL" : "PASS", id, detail);
}

// ---- transform group -------------------------------------------------------------------------

bool kindOf(const char* name, TransformKind& kind) {
  if (std::strcmp(name, "cw") == 0) kind = TransformKind::RotateCW;
  else if (std::strcmp(name, "ccw") == 0) kind = TransformKind::RotateCCW;
  else if (std::strcmp(name, "flipH") == 0) kind = TransformKind::FlipH;
  else if (std::strcmp(name, "flipV") == 0) kind = TransformKind::FlipV;
  else return false;
  return true;
}

const char* severityName(Severity s) { return s == Severity::Blocking ? "blocking" : s == Severity::Warning ? "warning" : "info"; }

/**
 * Each transform of a case (transforms.tsv): the report, every kept issue with its line text read
 * back, and the output by SHA-256, length, lines changed and summary. A mismatched output is
 * written again to `mismatch-<case>-<kind>.gcode`, when the platform can create files.
 */
void transformGroup(Platform& platform, Report& r, Scratch& sc, Arena arena, const char* id, const char* inputPath, const char* expectedPath) {
  LineReader* input = platform.open(inputPath);
  LineReader* expected = platform.open(expectedPath);
  if (!input || !expected) {
    if (input) platform.close(input);
    if (expected) platform.close(expected);
    return r.result("FAIL", id, "can't open the case's files");
  }
  Mismatch m;
  Fields& f = sc.group;
  Line l{};
  uint32_t runs = 0, issues = 0, outputs = 0;
  bool skipped = false;
  while (!m.any && !skipped && expected->next(l)) {
    f.load(l);  // T kind blocked warnings omitted count
    if (!f.is(0, "T")) continue;
    char kindName[8];
    std::snprintf(kindName, sizeof kindName, "%s", f[1]);
    TransformKind kind;
    if (!kindOf(kindName, kind)) {
      m.set("unknown transform %s%s", kindName, "");
      break;
    }
    const bool wantBlocked = f.is(2, "1"), wantWarnings = f.is(3, "1");
    const unsigned long wantOmitted = std::strtoul(f[4], nullptr, 10), wantCount = std::strtoul(f[5], nullptr, 10);
    gcode::Report rep{};
    CheckedTransform checked{};
    const Status s = check(*input, kind, TransformOptions{}, arena, rep, checked);
    if (s == Status::OutOfMemory) {
      skipped = true;
      break;
    }
    runs++;
    if (s != Status::Ok) {
      m.set("%s: check status %s", kindName, statusName(s));
      break;
    }
    if (rep.blocked != wantBlocked || rep.warnings != wantWarnings || rep.omitted != wantOmitted || rep.count != wantCount) {
      m.set("%s: the report's counts%s", kindName, "");
    }
    for (unsigned long k = 0; k < wantCount; k++) {
      if (!expected->next(l)) {
        m.set("%s: transforms.tsv ended%s", kindName, "");
        break;
      }
      f.load(l);  // I line severity ruleId choices reasonHex lineTextHex
      if (m.any || k >= rep.count) continue;
      const Issue& g = rep.issues[k];
      bool ok = f.is(0, "I") && std::strtoul(f[1], nullptr, 10) == g.line && f.is(2, severityName(g.severity)) && f.is(3, g.ruleId) &&
                f.is(4, g.hasChoices ? "1" : "0") && sameHexBytes(f[5], g.reason.data, g.reason.length);
      if (ok) {
        const int32_t n = reportLineText(*input, g, sc.text, sizeof sc.text);
        ok = n >= 0 && sameHexBytes(f[6], sc.text, static_cast<size_t>(n));
      }
      issues++;
      if (!ok) {
        char where[96];
        std::snprintf(where, sizeof where, "%s issue %lu (%s, line %s)", kindName, k, f[3], f[1]);
        m.set("%s%s", where, "");
      }
    }
    if (m.any) break;
    if (!expected->next(l)) {
      m.set("%s: no O line%s", kindName, "");
      break;
    }
    f.load(l);  // O sha256 length linesChanged summaryHex, or O - when blocked
    // applyChecked() from the check above first (apply() reuses the arena), then apply(): the
    // same bytes, which must be the expected ones.
    HashWriter viaChecked;
    TransformResult checkedRes{};
    const Status cs = applyChecked(*input, viaChecked, checked, FlipChoice::FlipOnly, TransformOptions{}, arena, checkedRes);
    HashWriter hw;
    TransformResult res{};
    const Status as = apply(*input, hw, kind, FlipChoice::FlipOnly, TransformOptions{}, arena, rep, res);
    if (as == Status::OutOfMemory || cs == Status::OutOfMemory) {
      skipped = true;
      break;
    }
    if (f.is(1, "-")) {
      if (as != Status::Blocked || hw.length != 0) m.set("%s: apply should block, got %s", kindName, statusName(as));
      if (cs != Status::Blocked || viaChecked.length != 0) m.set("%s: applyChecked should block, got %s", kindName, statusName(cs));
      continue;
    }
    outputs++;
    char digest[65], checkedDigest[65];
    hw.sha.hex(digest);
    viaChecked.sha.hex(checkedDigest);
    if (cs != Status::Ok || std::strcmp(digest, checkedDigest) != 0 || checkedRes.linesChanged != res.linesChanged ||
        std::strcmp(checkedRes.summary, res.summary) != 0) {
      m.set("%s: applyChecked differs from apply (%s)", kindName, statusName(cs));
    }
    const bool ok = as == Status::Ok && f.is(1, digest) && std::strtoull(f[2], nullptr, 10) == hw.length &&
                    std::strtoul(f[3], nullptr, 10) == res.linesChanged && sameHexBytes(f[4], res.summary, res.summaryLength);
    if (!ok) {
      m.set("%s: the output (apply %s)", kindName, statusName(as));
      char path[200];
      std::snprintf(path, sizeof path, "mismatch-%s-%s.gcode", id, kindName);
      if (ByteWriter* w = platform.create(path)) {
        apply(*input, *w, kind, FlipChoice::FlipOnly, TransformOptions{}, arena, rep, res);
        platform.closeWriter(w);
      }
    }
  }
  platform.close(input);
  platform.close(expected);
  if (skipped) return r.result("SKIP", id, "needs more memory than the arena");
  char detail[480];
  std::snprintf(detail, sizeof detail, "%u runs, %u issues, %u outputs%s%s", static_cast<unsigned>(runs), static_cast<unsigned>(issues),
                static_cast<unsigned>(outputs), m.any ? ", first mismatch: " : "", m.any ? m.text : "");
  r.result(m.any ? "FAIL" : "PASS", id, detail);
}

// ---- outline group ---------------------------------------------------------------------------

/** A digest of a result's loops (their starts and corners' bits) and summary, so two can be compared without a copy. */
void loopsDigest(const OutlineResult& res, char out[65]) {
  Sha256 sha;
  sha.update(reinterpret_cast<const char*>(&res.loopCount), sizeof res.loopCount);
  if (res.loopCount) {
    sha.update(reinterpret_cast<const char*>(res.loopStarts), (res.loopCount + 1) * sizeof(uint32_t));
    sha.update(reinterpret_cast<const char*>(res.corners), 2 * res.loopStarts[res.loopCount] * sizeof(double));
  }
  sha.update(res.summary, res.summaryLength);
  sha.hex(out);
}

/**
 * A case's Outline Check (outline.tsv): outlineCheck() against the report, every issue with its
 * line text read back, and the output by SHA-256, length, loops and summary; then the preview's
 * path, analyse() with AnalysisOptions::outline, whose outlineLoops() must be outlineCheck()'s and
 * whose writeOutline() must give the same bytes (15 §18.6).
 */
void outlineGroup(Platform& platform, Report& r, Scratch& sc, Arena arena, const char* id, const char* inputPath, const char* expectedPath) {
  LineReader* input = platform.open(inputPath);
  LineReader* expected = platform.open(expectedPath);
  if (!input || !expected) {
    if (input) platform.close(input);
    if (expected) platform.close(expected);
    return r.result("FAIL", id, "can't open the case's files");
  }
  Mismatch m;
  Fields& f = sc.group;
  Line l{};
  char name[256];
  size_t nameLength = 0;
  bool skipped = false;
  uint32_t issues = 0;
  // N sourceNameHex
  if (expected->next(l) && (f.load(l), f.is(0, "N"))) {
    const char* h = f[1];
    for (; h[0] && h[1] && nameLength < sizeof name; h += 2) {
      const char pair[3] = {h[0], h[1], '\0'};
      name[nameLength++] = static_cast<char>(std::strtoul(pair, nullptr, 16));
    }
  } else {
    m.set("outline.tsv has no N line%s", "");
  }
  // R blocked warnings omitted count
  if (!m.any && !(expected->next(l) && (f.load(l), f.is(0, "R")))) m.set("outline.tsv has no R line%s", "");
  const bool wantBlocked = f.is(1, "1"), wantWarnings = f.is(2, "1");
  const unsigned long wantOmitted = std::strtoul(f[3], nullptr, 10), wantCount = std::strtoul(f[4], nullptr, 10);
  gcode::Report rep{};
  OutlineResult res{};
  HashWriter hw;
  const Status s = m.any ? Status::Ok : outlineCheck(*input, hw, name, nameLength, OutlineOptions{}, arena, rep, res);
  if (s == Status::OutOfMemory) skipped = true;
  if (!m.any && !skipped) {
    if (s != Status::Ok && s != Status::Blocked) m.set("outlineCheck status %s%s", statusName(s));
    if (rep.blocked != wantBlocked || rep.warnings != wantWarnings || rep.omitted != wantOmitted || rep.count != wantCount) {
      m.set("the report's counts%s", "");
    }
  }
  for (unsigned long k = 0; !skipped && k < wantCount; k++) {
    if (!expected->next(l)) {
      m.set("outline.tsv ended%s", "");
      break;
    }
    f.load(l);  // I line severity ruleId choices reasonHex lineTextHex
    if (m.any || k >= rep.count) continue;
    const Issue& g = rep.issues[k];
    bool ok = f.is(0, "I") && std::strtoul(f[1], nullptr, 10) == g.line && f.is(2, severityName(g.severity)) && f.is(3, g.ruleId) &&
              f.is(4, g.hasChoices ? "1" : "0") && sameHexBytes(f[5], g.reason.data, g.reason.length);
    if (ok) {
      const int32_t n = reportLineText(*input, g, sc.text, sizeof sc.text);
      ok = n >= 0 && sameHexBytes(f[6], sc.text, static_cast<size_t>(n));
    }
    issues++;
    if (!ok) {
      char where[96];
      std::snprintf(where, sizeof where, "issue %lu (%s, line %s)", k, f[3], f[1]);
      m.set("%s%s", where, "");
    }
  }
  // O - | O empty | O sha256 length loops corners precision toleranceHex summaryHex
  if (!skipped && !m.any && !(expected->next(l) && (f.load(l), f.is(0, "O")))) m.set("outline.tsv has no O line%s", "");
  char ownDigest[65] = "", ownLoops[65] = "";
  if (!skipped && !m.any) {
    hw.sha.hex(ownDigest);
    if (f.is(1, "-")) {
      if (s != Status::Blocked || hw.length != 0) m.set("outlineCheck should block, got %s%s", statusName(s));
    } else if (f.is(1, "empty")) {
      if (s != Status::Ok || !res.empty || hw.length != 0) m.set("outlineCheck should be empty, got %s%s", statusName(s));
    } else {
      const uint32_t corners = res.loopCount ? res.loopStarts[res.loopCount] : 0;
      const bool ok = s == Status::Ok && f.is(1, ownDigest) && std::strtoull(f[2], nullptr, 10) == hw.length &&
                      std::strtoul(f[3], nullptr, 10) == res.loopCount && std::strtoul(f[4], nullptr, 10) == corners &&
                      std::strtoul(f[5], nullptr, 10) == res.precision && sameHex(f[6], res.tolerance) &&
                      sameHexBytes(f[7], res.summary, res.summaryLength);
      if (!ok) m.set("the output (outlineCheck %s)%s", statusName(s));
    }
    loopsDigest(res, ownLoops);  // before the arena is used again
  }
  // The preview's path: the same report, loops and bytes, with nothing read again.
  if (!skipped && !m.any) {
    Analysis a;
    AnalysisOptions ao;
    ao.outline = true;
    input->rewind();  // analyse() reads on from where the reader is
    const Status as = analyse(*input, ao, arena, nullptr, a);
    if (as == Status::OutOfMemory || a.outlineStatus == Status::OutOfMemory) {
      skipped = true;
    } else if (as != Status::Ok || a.outlineStatus != Status::Ok) {
      m.set("analyse with the outline: %s / %s", statusName(as), statusName(a.outlineStatus));
    } else {
      gcode::Report pr{};
      OutlineResult loops{};
      const Status rs = outlineReport(a, pr);
      const Status ls = outlineLoops(a, loops);
      char previewLoops[65] = "";
      if (ls == Status::Ok) loopsDigest(loops, previewLoops);
      HashWriter written;
      OutlineResult wres{};
      const Status ws = writeOutline(a, written, name, nameLength, arena, wres);
      char writtenDigest[65];
      written.sha.hex(writtenDigest);
      if (rs != Status::Ok || pr.count != rep.count || pr.blocked != rep.blocked) m.set("preview: the report (%s)%s", statusName(rs));
      else if (rep.blocked ? ls != Status::Blocked || ws != Status::Blocked : ls != Status::Ok || std::strcmp(previewLoops, ownLoops) != 0) {
        m.set("preview: outlineLoops() differs from outlineCheck()'s (%s)%s", statusName(ls));
      } else if (!rep.blocked && (ws != Status::Ok || written.length != hw.length || std::strcmp(writtenDigest, ownDigest) != 0)) {
        m.set("preview: writeOutline() differs (%s)%s", statusName(ws));
      }
    }
  }
  platform.close(input);
  platform.close(expected);
  if (skipped) return r.result("SKIP", id, "needs more memory than the arena");
  char detail[480];
  std::snprintf(detail, sizeof detail, "%u issues, %u loops%s%s", static_cast<unsigned>(issues), static_cast<unsigned>(res.loopCount),
                m.any ? ", first mismatch: " : "", m.any ? m.text : "");
  r.result(m.any ? "FAIL" : "PASS", id, detail);
}

// ---- surfacing group -------------------------------------------------------------------------

/** A hex field against text, "-" against none. */
bool sameHexText(const char* want, const char* got) {
  if (!got) return std::strcmp(want, "-") == 0;
  return sameHexBytes(want, got, std::strlen(got));
}

/** The arena's first kSurfacingBufferSize bytes: what a display gives the writer (15 §20.4). */
Arena smallArena(Arena arena) { return Arena{arena.base, arena.size < kSurfacingBufferSize ? arena.size : kSurfacingBufferSize}; }

/**
 * New Surfacing Job (surfacing.tsv, 15 §20.6): the form's data (M, B and F lines), each field
 * text's value and message (X lines), then each case (J lines): its field errors, or
 * planSurfacing()'s and writeSurfacingJob()'s name, laps, cutting length and summary, and the
 * file by SHA-256 and length. One result each for the form, the texts and the jobs.
 */
void surfacingGroup(Platform& platform, Report& r, Scratch& sc, Arena arena, const char* id, const char* path) {
  LineReader* in = platform.open(path);
  if (!in) return r.result("FAIL", id, "can't open the cases");
  Mismatch form, texts, jobs;
  uint32_t formLines = 0, textCount = 0, jobCount = 0;
  uint64_t written = 0;
  Fields& f = sc.group;
  Line l{};
  const Arena small = smallArena(arena);
  while (in->next(l)) {
    f.load(l);
    if (f.is(0, "M")) {
      formLines++;
      const SurfacingForm& m = surfacingForm();
      const char* got[] = {m.title, m.description, m.setup, m.bitLine, m.advancedTitle, m.probeLabel, m.probeSetup, m.keepZSetup};
      for (int k = 0; k < 8; k++) {
        if (!sameHexText(f[1 + k], got[k])) form.set("form text %s: %s", f[1 + k], got[k]);
      }
    } else if (f.is(0, "B")) {
      formLines++;
      const SurfacingBit& b = surfacingBit();
      if (!sameHexText(f[1], b.title) || !sameHexText(f[2], b.label) || !sameHexText(f[3], b.description) || !sameHex(f[4], b.diameter) ||
          !sameHex(f[5], b.maxRpm)) {
        form.set("the bit%s%s", "", "");
      }
    } else if (f.is(0, "F")) {
      formLines++;
      const int k = std::atoi(f[1]);
      if (k < 0 || k >= kSurfacingFieldCount) {
        form.set("field %s%s", f[2], "");
        continue;
      }
      const SurfacingFieldInfo& fi = surfacingField(static_cast<SurfacingField>(k));
      if (!f.is(2, fi.name) || !sameHexText(f[3], fi.label) || !sameHexText(f[4], fi.fieldUnit) || f.is(5, "1") != fi.advanced ||
          !sameHex(f[6], fi.min) || !sameHex(f[7], fi.max) || !sameHex(f[8], fi.defaultValue) || !f.is(9, fi.defaultText)) {
        form.set("field %s%s", f[2], "");
      }
    } else if (f.is(0, "X")) {
      // X field textHex valueBits messageHex|-
      textCount++;
      const int k = std::atoi(f[1]);
      if (k < 0 || k >= kSurfacingFieldCount) {
        texts.set("field %s%s", f[1], "");
        continue;
      }
      char* text = sc.text;
      size_t n = 0;
      for (const char* h = f[2]; h[0] && h[1] && n < sizeof sc.text; h += 2) {
        const char pair[3] = {h[0], h[1], 0};
        text[n++] = static_cast<char>(std::strtoul(pair, nullptr, 16));
      }
      const double v = fieldValue(text, n);
      SurfacingParams p;
      setSurfacingValue(p, static_cast<SurfacingField>(k), v);
      const char* messages[kSurfacingFieldCount];
      surfacingErrors(p, messages);
      if (!sameHex(f[3], v)) texts.set("text %s: value %s", f[2], f[3]);
      else if (!sameHexText(f[4], messages[k])) texts.set("text %s: message %s", f[2], f[4]);
    } else if (f.is(0, "J")) {
      // J name p0..pN probe E m0..mN, or J name p0..pN probe O sha256 length laps cutLength name summaryHex (N + 1 fields)
      jobCount++;
      constexpr int kProbe = 2 + kSurfacingFieldCount;  // 1, or 0 to keep Z zero
      constexpr int kMark = kProbe + 1;                 // the E or O column
      constexpr int kOut = kMark + 1;
      SurfacingParams p;
      for (int k = 0; k < kSurfacingFieldCount; k++) setSurfacingValue(p, static_cast<SurfacingField>(k), fromHex(f[2 + k]));
      p.probe = !f.is(kProbe, "0");
      const char* messages[kSurfacingFieldCount];
      const uint32_t bits = surfacingErrors(p, messages);
      SurfacingPlan plan{}, wplan{};
      HashWriter hw;
      const Status ps = planSurfacing(p, plan);
      const Status ws = writeSurfacingJob(p, hw, small, wplan);
      if (f.is(kMark, "E")) {
        bool ok = bits != 0 && ps == Status::InvalidParameters && ws == Status::InvalidParameters && hw.length == 0;
        for (int k = 0; k < kSurfacingFieldCount; k++) ok = ok && sameHexText(f[kOut + k], messages[k]);
        if (!ok) jobs.set("%s: the errors%s", f[1]);
        continue;
      }
      char digest[65];
      hw.sha.hex(digest);
      if (ps != Status::Ok || ws != Status::Ok) {
        jobs.set("%s: status %s", f[1], statusName(ps != Status::Ok ? ps : ws));
      } else if (!f.is(kOut, digest) || std::strtoull(f[kOut + 1], nullptr, 10) != hw.length) {
        jobs.set("%s: the file (%s)", f[1], digest);
      } else if (std::strtoul(f[kOut + 2], nullptr, 10) != plan.laps || !sameHex(f[kOut + 3], plan.cutLength)) {
        jobs.set("%s: laps or cutLength%s", f[1]);
      } else if (!f.is(kOut + 4, plan.name) || !sameHexText(f[kOut + 5], plan.summary)) {
        jobs.set("%s: name or summary %s", f[1], plan.summary);
      } else if (wplan.laps != plan.laps || !sameDouble(wplan.cutLength, plan.cutLength) || std::strcmp(wplan.name, plan.name) != 0 ||
                 std::strcmp(wplan.summary, plan.summary) != 0) {
        jobs.set("%s: planSurfacing() differs from writeSurfacingJob()%s", f[1]);
      } else {
        written += hw.length;
      }
    }
  }
  platform.close(in);
  char cid[160], detail[480];
  std::snprintf(cid, sizeof cid, "%s.form", id);
  std::snprintf(detail, sizeof detail, "%u lines%s%s", static_cast<unsigned>(formLines), form.any ? ", first mismatch: " : "", form.any ? form.text : "");
  r.result(form.any || formLines != 2 + kSurfacingFieldCount ? "FAIL" : "PASS", cid, detail);
  std::snprintf(cid, sizeof cid, "%s.texts", id);
  std::snprintf(detail, sizeof detail, "%u texts%s%s", static_cast<unsigned>(textCount), texts.any ? ", first mismatch: " : "", texts.any ? texts.text : "");
  r.result(texts.any || textCount == 0 ? "FAIL" : "PASS", cid, detail);
  std::snprintf(cid, sizeof cid, "%s.jobs", id);
  std::snprintf(detail, sizeof detail, "%u cases, %llu bytes written%s%s", static_cast<unsigned>(jobCount), static_cast<unsigned long long>(written),
                jobs.any ? ", first mismatch: " : "", jobs.any ? jobs.text : "");
  r.result(jobs.any || jobCount == 0 ? "FAIL" : "PASS", cid, detail);
}

/** The largest surfacing job (15 §20.5): planned, written to a hash, then to a scratch file when the platform has one. */
void surfacingPerf(Platform& platform, Report& r, Arena arena, const char* id) {
  SurfacingParams p;
  p.width = 3000;
  p.height = 3000;
  p.stepover = 10;
  SurfacingPlan plan{};
  uint32_t t0 = platform.millis();
  const Status ps = planSurfacing(p, plan);
  const uint32_t planMs = platform.millis() - t0;
  const Arena small = smallArena(arena);
  HashWriter hashed;
  t0 = platform.millis();
  const Status hs = writeSurfacingJob(p, hashed, small, plan);
  const uint32_t hashMs = platform.millis() - t0;
  char detail[480];
  int n = std::snprintf(detail, sizeof detail, "3000 x 3000 mm at 10%%: %u laps, %llu bytes; plan %u ms, write to a hash %u ms",
                        static_cast<unsigned>(plan.laps), static_cast<unsigned long long>(hashed.length), static_cast<unsigned>(planMs),
                        static_cast<unsigned>(hashMs));
  Status fs = Status::Ok;
  if (ByteWriter* w = platform.create("perf-surfacing.tmp")) {
    t0 = platform.millis();
    fs = writeSurfacingJob(p, *w, small, plan);
    platform.closeWriter(w);
    const uint32_t fileMs = platform.millis() - t0;
    if (n > 0 && static_cast<size_t>(n) < sizeof detail) {
      std::snprintf(detail + n, sizeof detail - static_cast<size_t>(n), ", to a file %u ms (%s)", static_cast<unsigned>(fileMs), statusName(fs));
    }
  }
  r.result(ps == Status::Ok && hs == Status::Ok && fs == Status::Ok ? "INFO" : "FAIL", id, detail);
}

// ---- perf group ------------------------------------------------------------------------------

void perfGroup(Platform& platform, Report& r, Scratch& sc, Arena arena, const char* id, const char* inputPath) {
  // Read rate: the whole file through the platform's reader.
  LineReader* in = platform.open(inputPath);
  if (!in) return r.result("FAIL", id, "can't open the input");
  Line l{};
  // Content bytes: each line's bytes plus one per line ending, so a CRLF counts as one and
  // the figure is less than the file size for CRLF files.
  uint64_t bytes = 0;
  uint32_t t0 = platform.millis();
  while (in->next(l)) bytes += l.length + 1;
  const uint32_t readMs = platform.millis() - t0;
  in->rewind();
  Analysis a;
  t0 = platform.millis();
  const Status s = analyse(*in, AnalysisOptions{}, arena, nullptr, a);
  const uint32_t analyseMs = platform.millis() - t0;
  if (s == Status::OutOfMemory) {
    platform.close(in);
    return r.result("SKIP", id, "needs more memory than the arena");
  }
  // Rotate CW: check, then applyChecked (the write pass alone) and apply (which checks again),
  // each into a hash.
  gcode::Report rep{};
  TransformResult res{};
  HashWriter hashed, hashedChecked;
  CheckedTransform checked{};
  t0 = platform.millis();
  const Status cs = check(*in, TransformKind::RotateCW, TransformOptions{}, arena, rep, checked);
  const uint32_t checkMs = platform.millis() - t0;
  t0 = platform.millis();
  const Status xs = cs == Status::Ok ? applyChecked(*in, hashedChecked, checked, FlipChoice::FlipOnly, TransformOptions{}, arena, res) : cs;
  const uint32_t checkedMs = platform.millis() - t0;
  t0 = platform.millis();
  const Status as = cs == Status::Ok ? apply(*in, hashed, TransformKind::RotateCW, FlipChoice::FlipOnly, TransformOptions{}, arena, rep, res) : cs;
  const uint32_t applyMs = platform.millis() - t0;
  const size_t transformArena = rep.arenaUsed;
  platform.close(in);
  // Write rate: 256 KB to a scratch file, when the platform offers one.
  uint32_t writeMs = 0;
  uint32_t written = 0;
  if (ByteWriter* w = platform.create("perf-write.tmp")) {
    std::memset(sc.text, 'G', sizeof sc.text);
    t0 = platform.millis();
    for (int k = 0; k < 64 && w->write(sc.text, sizeof sc.text); k++) written += sizeof sc.text;
    platform.closeWriter(w);
    writeMs = platform.millis() - t0;
  }
  char detail[480];
  std::snprintf(detail, sizeof detail,
                "%llu content bytes, %u lines: read %u ms, analyse %u ms, arena %lu bytes; Rotate CW %s: check %u ms, applyChecked %u ms, apply %u ms, arena %lu bytes; "
                "write %u bytes in %u ms",
                static_cast<unsigned long long>(bytes), static_cast<unsigned>(a.lineCount), static_cast<unsigned>(readMs),
                static_cast<unsigned>(analyseMs), static_cast<unsigned long>(a.arenaUsed), statusName(as), static_cast<unsigned>(checkMs),
                static_cast<unsigned>(checkedMs), static_cast<unsigned>(applyMs), static_cast<unsigned long>(transformArena), static_cast<unsigned>(written), static_cast<unsigned>(writeMs));
  const bool same = as != Status::Ok || (xs == Status::Ok && hashed.length == hashedChecked.length);
  r.result(s == Status::Ok && (as == Status::Ok || as == Status::Blocked) && same ? "INFO" : "FAIL", id, detail);

  // The Outline Check (15 §18.5): analyse() with the outline, Finish on its own (from its first
  // progress call, which passes the line count, to the return), the arena's peak and the bytes the
  // outline holds after; then outlineCheck() on its own, with its write.
  in = platform.open(inputPath);
  if (!in) return;
  struct FinishClock {
    Platform* p;
    uint32_t lines;
    uint32_t at;
    bool started;
  } clock{&platform, a.lineCount, 0, false};
  AnalysisOptions ao;
  ao.outline = true;
  ao.progressContext = &clock;
  ao.progress = [](void* ctx, uint32_t line, uint64_t) {
    auto* c = static_cast<FinishClock*>(ctx);
    if (line == c->lines && !c->started) {
      c->started = true;
      c->at = c->p->millis();
    }
    return true;
  };
  Analysis o;
  t0 = platform.millis();
  const Status os = analyse(*in, ao, arena, nullptr, o);
  const uint32_t outlineMs = platform.millis() - t0;
  const uint32_t finishMs = clock.started ? platform.millis() - clock.at : 0;
  OutlineResult loops{};
  const Status ls = os == Status::Ok ? outlineLoops(o, loops) : os;
  const size_t peak = o.arenaPeak, held = o.outlineHeld;
  HashWriter outlineFile;
  OutlineResult res2{};
  t0 = platform.millis();
  const Status cs2 = os == Status::Ok ? outlineCheck(*in, outlineFile, "perf.nc", 7, OutlineOptions{}, arena, rep, res2) : os;
  const uint32_t checkMs2 = platform.millis() - t0;
  platform.close(in);
  char oid[160];
  std::snprintf(oid, sizeof oid, "%s.outline", id);
  std::snprintf(detail, sizeof detail,
                "analyse with the outline %s/%s %u ms, Finish %u ms, peak %lu bytes, held %lu bytes, %u loops; outlineCheck %s %u ms, %llu bytes written",
                statusName(os), statusName(o.outlineStatus), static_cast<unsigned>(outlineMs), static_cast<unsigned>(finishMs),
                static_cast<unsigned long>(peak), static_cast<unsigned long>(held), static_cast<unsigned>(ls == Status::Ok ? loops.loopCount : 0),
                statusName(cs2), static_cast<unsigned>(checkMs2), static_cast<unsigned long long>(outlineFile.length));
  const bool outlineOk = os == Status::Ok && (o.outlineStatus == Status::Ok || o.outlineStatus == Status::OutOfMemory) &&
                         (cs2 == Status::Ok || cs2 == Status::Blocked || cs2 == Status::OutOfMemory);
  r.result(outlineOk ? "INFO" : "FAIL", oid, detail);
}

}  // namespace

Totals run(Platform& platform, Arena arena, const char* groups) {
  Report r(platform);
  // The scratch takes the arena's front, aligned; the library gets the rest.
  const uintptr_t base = reinterpret_cast<uintptr_t>(arena.base);
  const uintptr_t start = (base + alignof(Scratch) - 1) & ~static_cast<uintptr_t>(alignof(Scratch) - 1);
  const size_t need = static_cast<size_t>(start - base) + sizeof(Scratch);
  if (!arena.base || arena.size < need) {
    platform.print("FAIL the arena is smaller than the runner's scratch");
    r.totals.failed++;
    return r.totals;
  }
  Scratch& sc = *new (reinterpret_cast<void*>(start)) Scratch();
  arena = Arena{reinterpret_cast<char*>(start) + sizeof(Scratch), arena.size - need};
  LineReader* manifest = platform.open("manifest.tsv");
  if (!manifest) {
    platform.print("FAIL manifest.tsv can't be opened");
    r.totals.failed++;
    return r.totals;
  }
  // Each case runs as its manifest line is read: the manifest stays open beside the case's
  // own files (three files at most).
  char source[64] = "?";
  Line l{};
  Fields& f = sc.manifest;
  while (manifest->next(l)) {
    f.load(l);
    if (f.is(0, "source")) std::snprintf(source, sizeof source, "%s", f[1]);
    if (!f.is(0, "case") || !wanted(groups, f[2])) continue;
    const char* id = f[1];
    const char* group = f[2];
    if (std::strcmp(group, "float") == 0) floatGroup(platform, r, sc.group, id, f[3]);
    else if (std::strcmp(group, "parse") == 0) parseGroup(platform, r, sc.group, id, f[3]);
    else if (std::strcmp(group, "self") == 0) selfGroup(r, arena);
    else if (std::strcmp(group, "analyse") == 0) analyseGroup(platform, r, sc, arena, id, f[3], f[4]);
    else if (std::strcmp(group, "transform") == 0) transformGroup(platform, r, sc, arena, id, f[3], f[4]);
    else if (std::strcmp(group, "outline") == 0) outlineGroup(platform, r, sc, arena, id, f[3], f[4]);
    else if (std::strcmp(group, "surfacing") == 0) surfacingGroup(platform, r, sc, arena, id, f[3]);
    else if (std::strcmp(group, "perf") == 0 && f.is(1, "perf.surfacing")) surfacingPerf(platform, r, arena, id);
    else if (std::strcmp(group, "perf") == 0) perfGroup(platform, r, sc, arena, id, f[3]);
    else r.result("SKIP", id, "unknown group");
  }
  platform.close(manifest);
  char summary[256];
  std::snprintf(summary, sizeof summary, "SUMMARY passed %u failed %u skipped %u, gcode-core %s, source %s", static_cast<unsigned>(r.totals.passed),
                static_cast<unsigned>(r.totals.failed), static_cast<unsigned>(r.totals.skipped), version(), source);
  platform.print(summary);
  return r.totals;
}

}  // namespace gcode_device
