// New Surfacing Job (specs/14, 15 §20): packages/core/src/gen/surfacing.ts and gen/fields.ts,
// line for line where the arithmetic matters. The spiral is worked out in fixed point
// (thousandths of a mm) as doubles, as TS has it; each point comes from the lap number alone, so
// the writer keeps only the last point and the running length, and nothing grows.
#include "gcode/surfacing.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

#include "arena.h"
#include "gcode/num.h"

namespace gcode {
namespace {

/** Thousandths of a millimetre. */
constexpr int P = 3;

// The lowrider_post preamble and postamble values (11 §4.1), as that post writes them.
constexpr int kSafeZ = 5;
constexpr int kProbeRetractZ = 15;
constexpr int kRapidFeed = 2400;

const SurfacingBit kBit = {
    "1/2in 14\xc2\xb0 dovetail (Freud 22-104)",
    "T1 1/2in 14deg dovetail",
    "1/2in 14deg dovetail, Freud 22-104",
    12.7,
    24000,
};

const SurfacingFieldInfo kFields[kSurfacingFieldCount] = {
    {"width", "Width (X)", "mm", false, 1, 3000, 100, "100"},
    {"height", "Height (Y)", "mm", false, 1, 3000, 100, "100"},
    {"depth", "Depth", "mm", false, 0.01, 3, 0.1, "0.1"},
    // And no more than the width (height), checked in surfacingErrors(): 3000 is the most it can be.
    {"probeX", "Probe X", "mm", false, 0, 3000, 10, "10"},
    {"probeY", "Probe Y", "mm", false, 0, 3000, 10, "10"},
    // Above 80% the corners of a rectangular spiral can leave uncut points (14 §3).
    {"stepover", "Stepover", "% of bit", true, 10, 80, 40, "40"},
    {"feed", "Feed", "mm/min", true, 10, 10000, 1200, "1200"},
    {"plunge", "Plunge", "mm/min", true, 10, 5000, 300, "300"},
    {"spindle", "Spindle", "rpm", true, 1000, 24000, 18000, "18000"},
};

/** surfacingErrors' "Between MIN and MAX UNIT", with the limits as JS writes them; the probe's own. */
const char* const kRangeMessages[kSurfacingFieldCount] = {
    "Between 1 and 3000 mm",      "Between 1 and 3000 mm",       "Between 0.01 and 3 mm",      "Between 0 and the width",
    "Between 0 and the height",   "Between 10 and 80%",          "Between 10 and 10000 mm/min", "Between 10 and 5000 mm/min",
    "Between 1000 and 24000 rpm",
};

const SurfacingForm kForm = {
    "New Surfacing Job",
    "Skims the area in one pass with an inward clockwise spiral, so the cut is conventional.",
    "Zero X/Y at the area's lower-left corner; the bit hangs 6.35 mm past every edge.",
    "Bit: 1/2in 14\xc2\xb0 dovetail (Freud 22-104), 12.7 mm at the tip",
    "Stepover and feeds",
    "Probe the surface for Z zero",
    "Probe on a high spot, measured from that corner.",
    "Z zero stays as set, for example by an earlier job that probed, so this one cuts at the same height: for surfacing a piece "
    "in stages.",
};

/** Text into a fixed buffer; `ok` false once something didn't fit. */
struct Text {
  char* p;
  size_t cap;
  size_t len = 0;
  bool ok = true;
  void put(const char* s, size_t n) {
    if (len + n > cap) {
      ok = false;
      return;
    }
    std::memcpy(p + len, s, n);
    len += n;
  }
  void put(const char* s) { put(s, std::strlen(s)); }
  /** formatFixed(n, p). */
  void fixed(double n, int precision = P) {
    char tmp[32];
    const int k = formatFixed(n, precision, true, tmp, sizeof tmp);
    if (k < 0) ok = false;
    else put(tmp, static_cast<size_t>(k));
  }
  /** mm(v): formatFixed(toFixed(v, P), P). */
  void mm(double v) { fixed(toFixed(v, P)); }
};

/** The values the job is built from (buildSurfacingJob's first lines). */
struct Job {
  double w, h, step, half, n;
  explicit Job(const SurfacingParams& p) {
    w = toFixed(p.width, P);
    h = toFixed(p.height, P);
    const double s = toFixed((kBit.diameter * p.stepover) / 100, P);
    step = s > 1 ? s : 1;
    half = std::floor((w < h ? w : h) / 2);
    const double laps = -std::floor(-(half / step));  // Math.ceil
    n = laps > 1 ? laps : 1;
  }
  double inset(double k) const { return jsRound((k * half) / n); }

  /**
   * surfacingSpiral's path after its first point (0, 0), each point given to `to` once, a repeat
   * of the last one skipped, in the order TS pushes them.
   */
  template <typename F>
  void spiral(F&& visit) const {
    double lx = 0, ly = 0;
    const auto to = [&](double x, double y) {
      if (lx != x || ly != y) {
        visit(lx, ly, x, y);
        lx = x;
        ly = y;
      }
    };
    for (double k = 0; k < n; k++) {
      const double i = inset(k);
      const double next = inset(k + 1);
      to(i, h - i);
      to(w - i, h - i);
      to(w - i, i);
      to(next, i);
    }
    to(half, h - half);
    to(w - half, h - half);
  }
};

/** The name and summary from the job's facts (buildSurfacingJob's return). */
bool finishPlan(const SurfacingParams& p, const Job& job, double cutLength, SurfacingPlan& plan) {
  plan.laps = static_cast<uint32_t>(job.n + 1);
  plan.cutLength = cutLength;
  Text name{plan.name, sizeof plan.name - 1};
  name.put("surface-");
  name.fixed(job.w);
  name.put("x");
  name.fixed(job.h);
  name.put("-");
  name.mm(p.depth);
  name.put(p.probe ? "mm.gcode" : "mm-keepz.gcode");
  const double r = jsRound(cutLength / p.feed);
  const double minutes = r > 1 ? r : 1;
  Text s{plan.summary, sizeof plan.summary - 1};
  s.fixed(job.w);
  s.put(" \xc3\x97 ");  // ×
  s.fixed(job.h);
  s.put(" mm at ");
  s.mm(p.depth);
  s.put(" mm \xc2\xb7 ");  // ·
  s.fixed(job.n + 1, 0);
  s.put(" laps, ");
  if (cutLength >= 1000) {
    char tmp[32];
    const int k = formatDecimal(cutLength / 1000, 1, true, tmp, sizeof tmp);
    if (k < 0) s.ok = false;
    else s.put(tmp, static_cast<size_t>(k));
    s.put(" m");
  } else {
    s.fixed(jsRound(cutLength), 0);
    s.put(" mm");
  }
  s.put(" of cutting, about ");
  s.fixed(minutes, 0);
  s.put(" min");
  plan.name[name.len] = '\0';
  plan.summary[s.len] = '\0';
  plan.nameLength = static_cast<uint32_t>(name.len);
  plan.summaryLength = static_cast<uint32_t>(s.len);
  return name.ok && s.ok;
}

/** Lines through a buffer to the writer. */
struct Out {
  ByteWriter& writer;
  char* buf;
  size_t cap;
  Text line{nullptr, 0};
  size_t used = 0;
  bool failed = false;
  /** The longest line the job writes is under 160 bytes. */
  static constexpr size_t kLine = 256;
  Text& begin() {
    if (cap - used < kLine) flush();
    line = Text{buf + used, kLine};
    return line;
  }
  void end() {
    line.put("\n", 1);
    if (!line.ok) failed = true;
    used += line.len;
  }
  void put(const char* s) {
    begin().put(s);
    end();
  }
  void flush() {
    if (used > 0 && !failed && !writer.write(buf, used)) failed = true;
    used = 0;
  }
};

}  // namespace

const SurfacingFieldInfo& surfacingField(SurfacingField field) { return kFields[static_cast<int>(field)]; }

double surfacingValue(const SurfacingParams& p, SurfacingField field) {
  switch (field) {
    case SurfacingField::Width: return p.width;
    case SurfacingField::Height: return p.height;
    case SurfacingField::Depth: return p.depth;
    case SurfacingField::ProbeX: return p.probeX;
    case SurfacingField::ProbeY: return p.probeY;
    case SurfacingField::Stepover: return p.stepover;
    case SurfacingField::Feed: return p.feed;
    case SurfacingField::Plunge: return p.plunge;
    case SurfacingField::Spindle: return p.spindle;
  }
  return NAN;
}

void setSurfacingValue(SurfacingParams& p, SurfacingField field, double value) {
  switch (field) {
    case SurfacingField::Width: p.width = value; break;
    case SurfacingField::Height: p.height = value; break;
    case SurfacingField::Depth: p.depth = value; break;
    case SurfacingField::ProbeX: p.probeX = value; break;
    case SurfacingField::ProbeY: p.probeY = value; break;
    case SurfacingField::Stepover: p.stepover = value; break;
    case SurfacingField::Feed: p.feed = value; break;
    case SurfacingField::Plunge: p.plunge = value; break;
    case SurfacingField::Spindle: p.spindle = value; break;
  }
}

const SurfacingForm& surfacingForm() { return kForm; }

const SurfacingBit& surfacingBit() { return kBit; }

double fieldValue(const char* text, size_t length) {
  size_t start = 0, end = length;
  while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) end--;
  // ^[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)$
  size_t i = start;
  if (i < end && (text[i] == '+' || text[i] == '-')) i++;
  size_t digits = 0, points = 0;
  for (; i < end; i++) {
    const char c = text[i];
    if (c >= '0' && c <= '9') digits++;
    else if (c == '.' && points == 0) points++;
    else return NAN;
  }
  if (digits == 0) return NAN;
  return parseDecimal(text + start, end - start);
}

uint32_t surfacingErrors(const SurfacingParams& p, const char* messages[kSurfacingFieldCount]) {
  uint32_t bits = 0;
  for (int f = 0; f < kSurfacingFieldCount; f++) {
    const double v = surfacingValue(p, static_cast<SurfacingField>(f));
    const char* m = nullptr;
    // A probe field also stays within its side, when that is a number (PROBE_SIDES).
    const SurfacingField field = static_cast<SurfacingField>(f);
    // Without the probe, the probe fields aren't used, so they aren't checked.
    if (!p.probe && (field == SurfacingField::ProbeX || field == SurfacingField::ProbeY)) {
      if (messages) messages[f] = nullptr;
      continue;
    }
    const double side = field == SurfacingField::ProbeX ? p.width : field == SurfacingField::ProbeY ? p.height : NAN;
    if (!std::isfinite(v)) m = "Enter a number";
    else if (v < kFields[f].min || v > kFields[f].max || (std::isfinite(side) && v > side)) m = kRangeMessages[f];
    if (m) bits |= 1u << f;
    if (messages) messages[f] = m;
  }
  return bits;
}

Status planSurfacing(const SurfacingParams& p, SurfacingPlan& plan) {
  std::memset(&plan, 0, sizeof plan);
  if (surfacingErrors(p, nullptr) != 0) return Status::InvalidParameters;
  const Job job(p);
  double cutLength = 0;
  job.spiral([&](double px, double py, double x, double y) { cutLength += fmath::hypot(x - px, y - py) / 1000; });
  // The limits keep every number a safe integer, so the texts always fit.
  finishPlan(p, job, cutLength, plan);
  return Status::Ok;
}

Status writeSurfacingJob(const SurfacingParams& p, ByteWriter& writer, Arena arena, SurfacingPlan& plan) {
  std::memset(&plan, 0, sizeof plan);
  if (surfacingErrors(p, nullptr) != 0) return Status::InvalidParameters;
  ArenaAlloc alloc(arena);
  char* buf = static_cast<char*>(alloc.alloc(kSurfacingBufferSize, 1));
  if (!buf) return Status::OutOfMemory;
  const Job job(p);
  Out out{writer, buf, kSurfacingBufferSize};
  const SurfacingBit& bit = kBit;

  out.put("(Surfacing job, made by G-code Editor)");
  Text* t = &out.begin();
  t->put("(Bit: ");
  t->put(bit.description);
  t->put(", ");
  t->mm(bit.diameter);
  t->put(" mm at the tip)");
  out.end();
  t = &out.begin();
  t->put("(Area: ");
  t->fixed(job.w);
  t->put(" x ");
  t->fixed(job.h);
  t->put(" mm from where X/Y is zeroed; the bit overhangs every edge by ");
  t->mm(bit.diameter / 2);
  t->put(" mm)");
  out.end();
  t = &out.begin();
  t->put("(Depth ");
  t->mm(p.depth);
  t->put(p.probe ? " mm below the probed surface" : " mm below the Z zero already set");
  t->put(" in one pass: an inward clockwise spiral, conventional cutting)");
  out.end();
  t = &out.begin();
  t->put("(Stock: XMIN=0 XMAX=");
  t->fixed(job.w);
  t->put(" YMIN=0 YMAX=");
  t->fixed(job.h);
  t->put(" ZMIN=");
  t->mm(-p.depth);
  t->put(" ZMAX=0)");
  out.end();
  t = &out.begin();
  t->put("(ToolDef 1: SHAPE=flat D=");
  t->mm(bit.diameter);
  t->put(" NAME=");
  t->put(bit.label);
  t->put(")");
  out.end();
  for (const char* s : {"G21", "G90", "G94", "G54"}) out.put(s);
  if (p.probe) {
    // lowrider_post's probe (14 §4).
    out.put("G10 L20 P0 X0 Y0 Z0 (set current location to zero)");
    t = &out.begin();
    t->put("G0 X");
    t->mm(p.probeX);
    t->put(" Y");
    t->mm(p.probeY);
    t->put(" F");
    t->fixed(kRapidFeed, 0);
    t->put(" (move to probing location)");
    out.end();
    t = &out.begin();
    t->put("M0 (MSG Insert ");
    t->put(bit.label);
    t->put(", attach probe)");
    out.end();
    out.put("G38.2 Z-110 F200 P0.5 (probe down set thickness)");
    t = &out.begin();
    t->put("G1 Z");
    t->fixed(kProbeRetractZ, 0);
    t->put(" F900");
    out.end();
    out.put("M0 (MSG Remove probe and start router)");
  } else {
    // X/Y zero only, keeping Z zero, then above the surface (09 D74).
    out.put("G10 L20 P0 X0 Y0 (set current location to X/Y zero, keeping Z zero)");
    t = &out.begin();
    t->put("G0 Z");
    t->fixed(kProbeRetractZ, 0);
    t->put(" (above the surface)");
    out.end();
    t = &out.begin();
    t->put("M0 (MSG Z zero kept for ");
    t->put(bit.label);
    t->put(" - start router)");
    out.end();
  }
  t = &out.begin();
  t->put("M3 S");
  t->mm(p.spindle);
  out.end();
  out.put("G0 X0 Y0");
  t = &out.begin();
  t->put("G0 Z");
  t->fixed(kSafeZ, 0);
  out.end();
  t = &out.begin();
  t->put("G1 Z");
  t->mm(-p.depth);
  t->put(" F");
  t->mm(p.plunge);
  out.end();

  double cutLength = 0;
  bool first = true;
  job.spiral([&](double px, double py, double x, double y) {
    cutLength += fmath::hypot(x - px, y - py) / 1000;
    Text& l = out.begin();
    l.put("G1 X");
    l.fixed(x);
    l.put(" Y");
    l.fixed(y);
    if (first) {
      l.put(" F");
      l.mm(p.feed);
      first = false;
    }
    out.end();
  });

  t = &out.begin();
  t->put("G0 Z");
  t->fixed(kSafeZ, 0);
  out.end();
  for (const char* s : {"G0 X0 Y0", "M5", "G4 P0 ; wait for motion to finish before $ command", "$HZ", "M0 (MSG Job complete - stop router)", "M2"}) {
    out.put(s);
  }
  out.flush();
  finishPlan(p, job, cutLength, plan);
  return out.failed ? Status::WriteError : Status::Ok;
}

}  // namespace gcode
