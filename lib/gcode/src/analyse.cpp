// analyse() (specs/15 §5.2): two passes over the file.
//   Pass 1: refusals and line limits, the simulator (precision, extents, units), the operation
//           structure, stock comments, and the fixed positioning moves.
//   Pass 2: the simulator again, now with the final precision known: per-line states at the
//           final P, job extents, segments, and the per-line hook.
#include <cmath>
#include <cstring>

#include "arena.h"
#include "gcode/analysis.h"
#include "gcode/num.h"
#include "simulator.h"
#include "stock.h"
#include "structure.h"
#include "vocabulary.h"

namespace gcode {

// ---- Names (the TS tuples) -------------------------------------------------------------------

const char* motionName(Motion m) {
  static const char* const k[] = {"G0", "G1", "G2", "G3", "G38.2", "G38.3", "G38.4", "G38.5", "G80", "G73",
                                  "G76", "G81", "G82", "G83", "G84", "G85", "G86", "G87", "G88", "G89"};
  return k[static_cast<int>(m)];
}
const char* planeName(Plane p) { return p == Plane::G17 ? "G17" : p == Plane::G18 ? "G18" : "G19"; }
const char* distanceName(Distance d) { return d == Distance::G90 ? "G90" : "G91"; }
const char* arcDistanceName(ArcDistance d) { return d == ArcDistance::G90_1 ? "G90.1" : "G91.1"; }
const char* unitsModeName(UnitsMode u) { return u == UnitsMode::G20 ? "G20" : "G21"; }
const char* wcsName(Wcs w) {
  static const char* const k[] = {"initial", "G54", "G55", "G56", "G57", "G58", "G59"};
  return k[static_cast<int>(w)];
}
const char* feedModeName(FeedMode f) { return f == FeedMode::G93 ? "G93" : f == FeedMode::G94 ? "G94" : "G95"; }
const char* ownerName(Owner o) {
  static const char* const k[] = {"", "G10", "G28", "G30", "G92", "G53", "G38", "motion"};
  return k[static_cast<int>(o)];
}

namespace {

const char* const UTF16_MESSAGE = "UTF-16/32 files aren't supported. Save the file as UTF-8 or ASCII.";
const char* const BINARY_MESSAGE = "This looks like a binary file, not G-code.";

const uint32_t LETTER_X = 1u << ('X' - 'A'), LETTER_Y = 1u << ('Y' - 'A');

bool known(double v) { return !std::isnan(v); }

/** Whether the line has a probe G-code (G38.2–G38.5). */
bool hasProbe(const LineState& st) {
  static const int kProbes[] = {382, 383, 384, 385};
  for (int key : kProbes) {
    const int b = vocab::gIndex(key);
    if (b < 32 ? (st.gLo >> b) & 1u : (st.gHi >> (b - 32)) & 1u) return true;
  }
  return false;
}

/** Collects a line's tokens for the per-line hook. */
class TokenCollector final : public TokenSink {
 public:
  TokenCollector(Token* buf, uint32_t cap) : buf(buf), cap(cap) {}
  Token* buf;
  uint32_t cap;
  uint32_t count = 0;
  void add(TokenKind kind, uint32_t start, uint32_t end, char letter, CommentStyle style, uint32_t valueStart, int frac) {
    if (count >= cap) return;
    buf[count++] = Token{kind, letter, style, static_cast<uint8_t>(frac), static_cast<uint16_t>(start),
                         static_cast<uint16_t>(end - start), static_cast<uint16_t>(valueStart)};
  }
  void word(uint32_t s, uint32_t e, char l, uint32_t v, int f) override { add(TokenKind::Word, s, e, l, CommentStyle::None, v, f); }
  void space(uint32_t s, uint32_t e) override { add(TokenKind::Space, s, e, 0, CommentStyle::None, 0, 0); }
  void comment(uint32_t s, uint32_t e, CommentStyle st) override { add(TokenKind::Comment, s, e, 0, st, 0, 0); }
  void raw(uint32_t s, uint32_t e) override { add(TokenKind::Raw, s, e, 0, CommentStyle::None, 0, 0); }
};

/** The line as analyse() sees it: the UTF-8 BOM removed from the first line. */
struct LineText {
  const char* p;
  uint32_t n;
  uint64_t offset;
};

/** Reads lines, applying the checks every pass shares. */
class Passer {
 public:
  Passer(LineReader& r, const AnalysisOptions& o, Analysis& res) : reader(r), options(o), result(res) {}
  LineReader& reader;
  const AnalysisOptions& options;
  Analysis& result;
  Line line{};
  LineText text{};

  /** The next line, or false at the end / on a stop (result.status set). */
  bool next(bool firstPass) {
    if (!reader.next(line)) {
      if (reader.failed()) result.status = Status::ReadError;
      return false;
    }
    text = LineText{line.data, line.length, line.offset};
    if (line.index == 0) {
      const auto* b = reinterpret_cast<const unsigned char*>(line.data);
      const bool utf8 = line.length >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf;
      if (utf8) {
        text = LineText{line.data + 3, line.length - 3, line.offset + 3};
        result.utf8Bom = true;
        // A file of only the BOM has no lines: TS removes the BOM before splitting.
        if (text.n == 0 && line.ending == Ending::None) return false;
      } else if (firstPass && ((line.length >= 2 && ((b[0] == 0xff && b[1] == 0xfe) || (b[0] == 0xfe && b[1] == 0xff))) ||
                               (line.length >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0xfe && b[3] == 0xff))) {
        return stop(Status::Refused, UTF16_MESSAGE);
      }
    }
    if (firstPass && std::memchr(line.data, 0, line.length)) return stop(Status::Refused, BINARY_MESSAGE);
    if (text.n > options.maxLineLength) {
      result.longLine = line.index;
      return stop(Status::LineTooLong, nullptr);
    }
    if (line.index > 0 && line.index % kCheckpointLines == 0 && options.progress &&
        !options.progress(options.progressContext, line.index, line.offset)) {
      return stop(Status::Cancelled, nullptr);
    }
    return true;
  }

  bool stop(Status s, const char* message) {
    result.status = s;
    if (message) result.refusedMessage = message;
    return false;
  }
};

}  // namespace

Status analyse(LineReader& reader, const AnalysisOptions& options, Arena arenaIn, AnalysisSink* sink, Analysis& result) {
  std::memset(&result, 0, sizeof result);
  result.status = Status::Ok;
  result.refusedMessage = "";
  result.firstXYMotion = -1;
  ArenaAlloc arena(arenaIn);
  const AnalysisOptions opt = [&] {
    AnalysisOptions o = options;
    if (o.maxLineLength > 65535) o.maxLineLength = 65535;
    return o;
  }();
  const auto oom = [&] {
    result.status = Status::OutOfMemory;
    return result.status;
  };

  // Scratch: built texts (two names of the longest line) and the hook's tokens.
  const size_t scratchSize = 2 * static_cast<size_t>(opt.maxLineLength) + 256;
  char* scratch = arena.array<char>(scratchSize);
  Token* tokens = sink ? arena.array<Token>(opt.maxLineLength + 1) : nullptr;
  if (arena.failed) return oom();
  // The simulator, the line state and the structure finder are small (a few hundred bytes each).
  Simulator simulator;
  LineState state{};
  StructureFinder structureFinder(arena, scratch, scratchSize);
  Simulator* sim = &simulator;
  LineState* st = &state;
  StructureFinder* structure = &structureFinder;

  Vec<StockComment> stocks;
  Vec<FixedMove> fixed;

  // ---- Pass 1 ----
  Passer pass(reader, opt, result);
  bool pending = false, pendingProbe = false, pendingZero = false;
  uint32_t pendingLine = 0, parseFlags = 0;
  const auto resolve = [&](bool nextIsG0) {
    if (pending && nextIsG0 && (pendingProbe || pendingZero)) {
      if (!fixed.push(arena, FixedMove{pendingLine, pendingProbe ? FixedMoveKind::Probe : FixedMoveKind::Zero})) return false;
    }
    pending = false;
    return true;
  };
  while (pass.next(true)) {
    const LineText& t = pass.text;
    const uint32_t i = pass.line.index;
    parseFlags |= parseLineFlags(t.p, t.n);
    sim->step(t.p, t.n, i, *st);
    structure->step(t.p, t.n, i);
    if (!stockCommentsIn(t.p, t.n, i, arena, stocks)) return oom();

    // Fixed positioning moves (fixed-moves.ts), resolved at the next XY motion line.
    if (pending) {
      if (st->isXYMotion) {
        if (!resolve(st->motion == Motion::G0)) return oom();
      } else if (hasProbe(*st)) {
        pendingProbe = true;
      }
    }
    if (st->isXYMotion && st->motion == Motion::G0 && st->distance == Distance::G90 &&
        (st->letters & (LETTER_X | LETTER_Y)) == (LETTER_X | LETTER_Y) && st->owner == Owner::Motion) {
      pending = true;
      pendingLine = i;
      pendingProbe = false;
      pendingZero = st->after.x == 0 && st->after.y == 0;
    }
    result.lineCount = i + 1;
    if (arena.failed) return oom();
  }
  if (result.status != Status::Ok) return result.status;
  if (!resolve(true)) return oom();

  const int P = sim->precision();
  result.precision = static_cast<uint8_t>(P);
  result.firstXYMotion = sim->firstXYMotion();
  sim->finish(result.extents, result.displayExtents, result.units, result.unitsAssumed, result.unitsMixed);
  result.parseFlags = parseFlags;
  structure->finish(result.structure);
  if (arena.failed) return oom();
  result.stockComments = stocks.data;
  result.stockCommentCount = stocks.size;
  result.fixedMoves = fixed.data;
  result.fixedMoveCount = fixed.size;
  result.stockBox.present = false;
  if (stocks.size) stockBox(stocks.back(), P, result.stockBox);

  // The rises of pass 1, for bringing each line to the final precision.
  const int raiseCount = sim->raiseCount();
  Simulator::Raise raises[16];
  std::memcpy(raises, sim->raises(), sizeof(Simulator::Raise) * static_cast<size_t>(raiseCount));

  // ---- Pass 2 ----
  if (!reader.rewind()) {
    result.status = Status::ReadError;
    return result.status;
  }
  sim->reset();
  const Structure& s = result.structure;
  uint32_t fixedAt = 0, unitAt = 0, simFlags = 0;
  double jMinX = HUGE_VAL, jMaxX = -HUGE_VAL, jMinY = HUGE_VAL, jMaxY = -HUGE_VAL;
  const auto jobAdd = [&](double x, double y) {
    if (x < jMinX) jMinX = x;
    if (x > jMaxX) jMaxX = x;
    if (y < jMinY) jMinY = y;
    if (y > jMaxY) jMaxY = y;
  };
  while (pass.next(false)) {
    const LineText& t = pass.text;
    const uint32_t i = pass.line.index;
    sim->step(t.p, t.n, i, *st);
    st->parseFlags = parseLineFlags(t.p, t.n);
    // The values at the final precision, as TS's rescaling of stored positions leaves them.
    bool overflow = false, ignored = false;
    st->after.x = toFinal(st->after.x, i, raises, raiseCount, overflow);
    st->after.y = toFinal(st->after.y, i, raises, raiseCount, overflow);
    st->before.x = toFinal(st->before.x, i, raises, raiseCount, ignored);
    st->before.y = toFinal(st->before.y, i, raises, raiseCount, ignored);
    if (overflow) st->flags |= kPrecisionOverflow;
    st->precision = static_cast<uint8_t>(P);
    simFlags |= st->flags;

    while (fixedAt < fixed.size && fixed.data[fixedAt].line < i) fixedAt++;
    const bool isFixed = fixedAt < fixed.size && fixed.data[fixedAt].line == i;

    // Job extents (fixed-moves.ts jobExtents): every XY motion but the fixed moves.
    if (st->isXYMotion && !isFixed) {
      if (known(st->after.x) && known(st->after.y)) jobAdd(st->after.x, st->after.y);
      if (st->arc.state == ArcState::Ok) {
        for (int k = 0; k < st->arc.geometry.extremeCount; k++) jobAdd(toFixed(st->arc.geometry.extremeX[k], P), toFixed(st->arc.geometry.extremeY[k], P));
      }
    }

    if (sink) {
      // Segments: motion lines under G0–G3.
      const bool motionLine = static_cast<int>(st->motion) <= 3 && (st->owner == Owner::Motion || st->isXYMotion);
      if (motionLine) {
        const Position& b = st->before;
        const Position& a = st->after;
        if (!known(b.x) || !known(b.y) || !known(b.z) || !known(a.x) || !known(a.y) || !known(a.z)) {
          result.segmentsSkipped = true;
        } else {
          Segment g{};
          g.line = i;
          g.offset = t.offset;
          g.kind = static_cast<SegmentKind>(st->motion);
          g.plane = st->plane;
          g.x0 = toNumber(b.x, P);
          g.y0 = toNumber(b.y, P);
          g.z0 = b.z;
          g.x1 = toNumber(a.x, P);
          g.y1 = toNumber(a.y, P);
          g.z1 = a.z;
          g.cx = g.cy = g.cz = NAN;
          if (st->arc.state == ArcState::Ok) {
            g.cx = st->arc.geometry.cx;
            g.cy = st->arc.geometry.cy;
          }
          while (s.kind == StructureKind::Ok && unitAt < s.unitCount && s.units[unitAt].body.end <= i) unitAt++;
          g.unit = s.kind == StructureKind::Ok && unitAt < s.unitCount && s.units[unitAt].body.start <= i ? static_cast<int16_t>(unitAt) : -1;
          g.flags = 0;
          if ((st->units == UnitsMode::G20) != (result.units == Units::In)) g.flags |= kSegmentMixedUnits;
          if (isFixed) g.flags |= kSegmentFixedMove;
          sink->segment(g);
        }
      }
      TokenCollector tc(tokens, opt.maxLineLength + 1);
      scanLine(t.p, t.n, tc);
      const Line view{t.p, t.n, pass.line.ending, i, t.offset};
      sink->line(view, tokens, static_cast<uint16_t>(tc.count), *st);
    }
  }
  if (result.status != Status::Ok) return result.status;
  result.simFlags = simFlags;
  result.jobExtents.present = jMinX != HUGE_VAL;
  result.jobExtents.minX = jMinX;
  result.jobExtents.maxX = jMaxX;
  result.jobExtents.minY = jMinY;
  result.jobExtents.maxY = jMaxY;
  result.arenaUsed = arena.used();
  return arena.failed ? oom() : Status::Ok;
}

}  // namespace gcode
