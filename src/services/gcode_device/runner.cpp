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
#include "gcode/transform.h"
#include "gcode/version.h"

namespace gcode_device {
namespace {

using namespace gcode;

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
  static const char* const k[] = {"Ok", "Refused", "LineTooLong", "ReadError", "OutOfMemory", "Cancelled", "Blocked", "NotSupported", "WriteError", "FileChanged"};
  const int i = static_cast<int>(s);
  return i >= 0 && i < 10 ? k[i] : "?";
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

// ---- SHA-256 (FIPS 180-4): a transform's output against the TS bytes, without storing it ----

class Sha256 {
 public:
  Sha256() {
    static const uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, kInit, sizeof h_);
  }
  void update(const char* p, size_t n) {
    bits_ += static_cast<uint64_t>(n) * 8;
    while (n > 0) {
      const size_t k = n < 64 - used_ ? n : 64 - used_;
      std::memcpy(buf_ + used_, p, k);
      used_ += k;
      p += k;
      n -= k;
      if (used_ == 64) {
        block(buf_);
        used_ = 0;
      }
    }
  }
  /** The digest as 64 lowercase hex digits (ends the hash). */
  void hex(char out[65]) {
    const uint64_t bits = bits_;
    const char pad = static_cast<char>(0x80), zero = 0;
    update(&pad, 1);
    while (used_ != 56) update(&zero, 1);
    char len[8];
    for (int k = 0; k < 8; k++) len[k] = static_cast<char>(bits >> (56 - 8 * k));
    update(len, 8);
    static const char d[] = "0123456789abcdef";
    for (int k = 0; k < 32; k++) {
      const unsigned b = (h_[k / 4] >> (24 - 8 * (k % 4))) & 0xffu;
      out[2 * k] = d[b >> 4];
      out[2 * k + 1] = d[b & 15];
    }
    out[64] = '\0';
  }

 private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const unsigned char* b) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int t = 0; t < 16; t++) {
      w[t] = static_cast<uint32_t>(b[4 * t]) << 24 | static_cast<uint32_t>(b[4 * t + 1]) << 16 | static_cast<uint32_t>(b[4 * t + 2]) << 8 |
             static_cast<uint32_t>(b[4 * t + 3]);
    }
    for (int t = 16; t < 64; t++) {
      const uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
      const uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
      w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int t = 0; t < 64; t++) {
      const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + w[t];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = bb;
      bb = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += bb;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }
  uint32_t h_[8];
  unsigned char buf_[64];
  size_t used_ = 0;
  uint64_t bits_ = 0;
};

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
