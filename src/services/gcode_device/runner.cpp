// The device test suite's runner (specs/15 §9.4); the data format is in data/FORMAT.md of the
// suite (written by tools/package-core-cpp.mjs). Platform-free: only the library and <cstdio>,
// <cstdlib>, <cstring>, <cmath>.
#include "runner.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gcode/analysis.h"
#include "gcode/num.h"
#include "gcode/version.h"

namespace gcode_device {
namespace {

using namespace gcode;

// ---- Text helpers ----------------------------------------------------------------------------

/** A line copied out of the reader (its data is valid only until the next read), split on tabs. */
struct Fields {
  char buf[4096];
  const char* f[64];
  int n = 0;
  bool load(const Line& line) {
    const size_t len = line.length < sizeof(buf) - 1 ? line.length : sizeof(buf) - 1;
    std::memcpy(buf, line.data, len);
    buf[len] = '\0';
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
  const char* operator[](int i) const { return i < n ? f[i] : ""; }
  bool is(int i, const char* s) const { return std::strcmp((*this)[i], s) == 0; }
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

void floatGroup(Platform& platform, Report& r, const char* id, const char* file) {
  LineReader* in = platform.open(file);
  if (!in) return r.result("FAIL", id, "can't open the vectors");
  Line line{};
  static Fields f;  // static: the device task stack is small (about 8 KB)
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

void parseGroup(Platform& platform, Report& r, const char* id, const char* file) {
  LineReader* in = platform.open(file);
  if (!in) return r.result("FAIL", id, "can't open the vectors");
  Line line{};
  static Fields f;  // static: the device task stack is small (about 8 KB)
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
  StateCheck(LineReader* expected, Mismatch& m) : expected_(expected), m_(m) {}
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
  Mismatch& m_;
  static Fields f_;
};

Fields StateCheck::f_;

bool extentsMatch(const Fields& f, const Extents& e, bool hex) {
  if (f.is(2, "null")) return !e.present;
  if (!e.present) return false;
  const double v[4] = {e.minX, e.maxX, e.minY, e.maxY};
  for (int k = 0; k < 4; k++) {
    if (hex ? !sameHex(f[2 + k], v[k]) : !sameFixed(f[2 + k], v[k])) return false;
  }
  return true;
}

void analyseGroup(Platform& platform, Report& r, Arena arena, const char* id, const char* inputPath, const char* expectedPath) {
  LineReader* input = platform.open(inputPath);
  LineReader* expected = platform.open(expectedPath);
  if (!input || !expected) {
    if (input) platform.close(input);
    if (expected) platform.close(expected);
    return r.result("FAIL", id, "can't open the case's files");
  }
  Mismatch m;
  Line l{};
  static Fields f;  // static: the device task stack is small (about 8 KB)
  expected->next(l);
  f.load(l);  // H	states	0|1
  const bool states = f.is(0, "H") && f.is(2, "1");
  StateCheck check(expected, m);
  Analysis a;
  const Status s = analyse(*input, AnalysisOptions{}, arena, states ? &check : nullptr, a);
  if (s == Status::OutOfMemory) {
    platform.close(input);
    platform.close(expected);
    return r.result("SKIP", id, "needs more memory than the arena");
  }
  static char hexbuf[2048];
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
      toHex(st.reason.data, st.reason.length, hexbuf, sizeof hexbuf);
      bool ok = f.is(2, kind);
      if (ok && st.kind != StructureKind::Ok) ok = f.is(3, hexbuf) && std::strtod(f[4], nullptr) == st.reasonLine;
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

// ---- perf group ------------------------------------------------------------------------------

void perfGroup(Platform& platform, Report& r, Arena arena, const char* id, const char* inputPath) {
  // Read rate: the whole file through the platform's reader.
  LineReader* in = platform.open(inputPath);
  if (!in) return r.result("FAIL", id, "can't open the input");
  Line l{};
  uint64_t bytes = 0;
  uint32_t t0 = platform.millis();
  while (in->next(l)) bytes += l.length + 1;
  const uint32_t readMs = platform.millis() - t0;
  in->rewind();
  Analysis a;
  t0 = platform.millis();
  const Status s = analyse(*in, AnalysisOptions{}, arena, nullptr, a);
  const uint32_t analyseMs = platform.millis() - t0;
  platform.close(in);
  if (s == Status::OutOfMemory) return r.result("SKIP", id, "needs more memory than the arena");
  // Write rate: 256 KB to a scratch file, when the platform offers one.
  uint32_t writeMs = 0;
  uint32_t written = 0;
  if (ByteWriter* w = platform.create("perf-write.tmp")) {
    static char block[4096];
    std::memset(block, 'G', sizeof block);
    t0 = platform.millis();
    for (int k = 0; k < 64 && w->write(block, sizeof block); k++) written += sizeof block;
    platform.closeWriter(w);
    writeMs = platform.millis() - t0;
  }
  char detail[256];
  std::snprintf(detail, sizeof detail, "%llu bytes, %u lines: read %u ms, analyse %u ms, arena %lu bytes; write %u bytes in %u ms",
                static_cast<unsigned long long>(bytes), static_cast<unsigned>(a.lineCount), static_cast<unsigned>(readMs),
                static_cast<unsigned>(analyseMs), static_cast<unsigned long>(a.arenaUsed), static_cast<unsigned>(written), static_cast<unsigned>(writeMs));
  r.result(s == Status::Ok ? "INFO" : "FAIL", id, detail);
}

}  // namespace

Totals run(Platform& platform, Arena arena, const char* groups) {
  Report r(platform);
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
  static Fields f;  // static: the device task stack is small (about 8 KB)
  while (manifest->next(l)) {
    f.load(l);
    if (f.is(0, "source")) std::snprintf(source, sizeof source, "%s", f[1]);
    if (!f.is(0, "case") || !wanted(groups, f[2])) continue;
    const char* id = f[1];
    const char* group = f[2];
    if (std::strcmp(group, "float") == 0) floatGroup(platform, r, id, f[3]);
    else if (std::strcmp(group, "parse") == 0) parseGroup(platform, r, id, f[3]);
    else if (std::strcmp(group, "self") == 0) selfGroup(r, arena);
    else if (std::strcmp(group, "analyse") == 0) analyseGroup(platform, r, arena, id, f[3], f[4]);
    else if (std::strcmp(group, "perf") == 0) perfGroup(platform, r, arena, id, f[3]);
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
