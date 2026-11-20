// Reading a job (specs/15 §5.1–5.2, C1): the simulator's per-line state, motion segments for a
// preview, and analyse(), which fills what the TS core's simulate(), jobExtents(),
// fixedPositioningMoves(), findStockComments() and findOperationStructure() give.
#pragma once

#include <cstddef>
#include <cstdint>

#include "gcode/io.h"
#include "gcode/tokens.h"

/** The C++ core: reading a job, and (from later phases) Rotate, Flip and the Outline Check. */
namespace gcode {

// ---- Modal state (sim/simulator.ts). Each is an index into the TS tuple of the same name.

/** Motion mode (TS MOTIONS): G0 G1 G2 G3, the probes, G80, the canned cycles. */
enum class Motion : uint8_t { G0, G1, G2, G3, G38_2, G38_3, G38_4, G38_5, G80, G73, G76, G81, G82, G83, G84, G85, G86, G87, G88, G89 };
/** Arc plane (TS PLANES). */
enum class Plane : uint8_t { G17, G18, G19 };
/** Distance mode (TS DISTANCES). */
enum class Distance : uint8_t { G90, G91 };
/** Arc centre distance mode (TS ARC_DISTANCES). */
enum class ArcDistance : uint8_t { G90_1, G91_1 };
/** Units mode on the line (TS UNITS). */
enum class UnitsMode : uint8_t { G20, G21 };
/** Work coordinate system: none selected yet ("initial"), then G54–G59 (TS WCS). */
enum class Wcs : uint8_t { Initial, G54, G55, G56, G57, G58, G59 };
/** Feed rate mode (TS FEEDS). */
enum class FeedMode : uint8_t { G93, G94, G95 };
/** Who a line's X/Y/Z words belong to (04 §5), or None without axis words (TS OWNERS). */
enum class Owner : uint8_t { None, G10, G28, G30, G92, G53, G38, Motion };
/** A file's units (TS Units). */
enum class Units : uint8_t { Mm, In };

/** The TS name of a motion mode ("G38.2"), for logs and the golden comparison. */
const char* motionName(Motion m);
/** The TS name of a plane ("G17"). */
const char* planeName(Plane p);
/** The TS name of a distance mode ("G90"). */
const char* distanceName(Distance d);
/** The TS name of an arc distance mode ("G91.1"). */
const char* arcDistanceName(ArcDistance d);
/** The TS name of a units mode ("G21"). */
const char* unitsModeName(UnitsMode u);
/** The TS name of a WCS ("initial", "G54"). */
const char* wcsName(Wcs w);
/** The TS name of a feed mode ("G94"). */
const char* feedModeName(FeedMode f);
/** The TS name of an owner ("motion"); "" for None (TS null). */
const char* ownerName(Owner o);

/** A position: X and Y fixed-point at the state's precision, Z a float; NaN when unknown (TS null). */
struct Position {
  double x;  ///< X, fixed point (an integer scaled by 10^precision), or NaN.
  double y;  ///< Y, fixed point, or NaN.
  double z;  ///< Z, a float in the line's units, or NaN.
};

/** Arc geometry (sim/arc.ts ArcGeometry), floats in the line's units. */
struct ArcGeometry {
  double cx;          ///< Centre X.
  double cy;          ///< Centre Y.
  double r;           ///< Radius.
  double startAngle;  ///< atan2 of the start, in radians.
  /** Signed sweep: negative is clockwise; ±2π for a full circle. */
  double sweep;
  /** How many cardinal points lie strictly inside the sweep (0–4). */
  uint8_t extremeCount;
  double extremeX[4];  ///< The cardinal points' X (C ± r on one axis), the first extremeCount used.
  double extremeY[4];  ///< The cardinal points' Y.
};

/** Whether a line has an arc, and whether it could be interpreted. */
enum class ArcState : uint8_t {
  None,    ///< Not a G2/G3 line in G17 with a known start (TS null).
  Ok,      ///< Interpreted: ArcInfo::geometry holds it.
  Invalid  ///< Not interpretable: ArcInfo::reason says why.
};

/** A line's arc: none, its geometry, or why it has none (the TS reason text). */
struct ArcInfo {
  ArcState state;        ///< Which of the three.
  ArcGeometry geometry;  ///< For Ok.
  /** For Invalid: the TS reason text; not terminated. */
  char reason[160];
  uint8_t reasonLength;  ///< Bytes of reason used.
};

/** One line's state (sim/simulator.ts LineState), plus the per-line facts classifiers read. */
struct LineState {
  Motion motion;            ///< Motion mode after the line.
  Plane plane;              ///< Plane after the line.
  Distance distance;        ///< Distance mode after the line.
  ArcDistance arcDistance;  ///< Arc distance mode after the line.
  UnitsMode units;          ///< Units mode after the line.
  Wcs wcs;                  ///< WCS after the line.
  FeedMode feedMode;        ///< Feed mode after the line.
  Owner owner;              ///< Who the line's X/Y/Z words belong to.
  /** The scale of before/after's X and Y: 10^precision (15 §5.1). The final P outside the library. */
  uint8_t precision;
  /** XY motion under G0–G3 (Simulation.isXYMotion). */
  bool isXYMotion;
  Position before;  ///< The position before the line (the previous line's after).
  Position after;   ///< The position after the line.
  ArcInfo arc;      ///< The line's arc, if any.
  /** Simulation flags (LineFlag bits 10 and 12–18). */
  uint32_t flags;
  /** Parse flags (parseLineFlags). */
  uint32_t parseFlags;
  /** Bit per letter A–Z with a word on the line, G and M included. */
  uint32_t letters;
  /** Recognized G-codes on the line: bit b is G_CODE_ORDER[b] (b < 32). */
  uint32_t gLo;
  /** Recognized G-codes on the line: bit b is G_CODE_ORDER[b + 32]. */
  uint32_t gHi;
  /** Recognized M-codes: bit b is M_CODE_ORDER[b]. */
  uint32_t mBits;
};

// ---- Segments (15 §5.2) ----------------------------------------------------------------------

/** A segment's kind; a plunge is a Feed with XY unchanged. */
enum class SegmentKind : uint8_t {
  Rapid,   ///< G0.
  Feed,    ///< G1.
  ArcCW,   ///< G2.
  ArcCCW,  ///< G3.
};

/** Segment::flags bits. */
enum SegmentFlag : uint16_t {
  kSegmentUnknownPosition = 1u << 0,  ///< Reserved: segments with an unknown position aren't sent.
  kSegmentMixedUnits = 1u << 1,       ///< The line is in other units than the file's primary units.
  kSegmentFixedMove = 1u << 2,        ///< A fixed positioning move (probe location, return to zero).
};

/** One motion line, for a preview and progress tracking: derived from its LineState. */
struct Segment {
  uint32_t line;     ///< The line it came from (0-based).
  uint64_t offset;   ///< That line's byte offset.
  SegmentKind kind;  ///< Rapid, feed or arc.
  Plane plane;       ///< The arc plane: which two centre coordinates an arc uses.
  double x0;         ///< Start X (before), file units, work coordinates.
  double y0;         ///< Start Y.
  double z0;         ///< Start Z.
  double x1;         ///< End X (after).
  double y1;         ///< End Y.
  double z1;         ///< End Z.
  /** Arc centre X, for an interpreted G17 arc; NaN otherwise (TS has no geometry for G18/G19). */
  double cx;
  double cy;  ///< Arc centre Y, or NaN.
  double cz;  ///< Unused in G17; NaN.
  /** Index in the operation structure's units, -1 outside any. */
  int16_t unit;
  uint16_t flags;  ///< SegmentFlag bits.
};

/** Receives the final pass's lines and segments. Both are optional; the defaults do nothing. */
class AnalysisSink {
 public:
  /** One motion line under G0–G3 with a known position, in order (before its line() call). */
  virtual void segment(const Segment&) {}
  /** Every line, with its tokens and its state at the final precision (15 §5.2 per-line hook). */
  virtual void line(const Line&, const Token* /*tokens*/, uint16_t /*tokenCount*/, const LineState&) {}

 protected:
  ~AnalysisSink() = default;
};

// ---- Results ---------------------------------------------------------------------------------

/** Bounds (TS Extents): fixed point at the result's precision, or floats where stated. */
struct Extents {
  bool present;  ///< False where TS has null.
  double minX;   ///< Lowest X.
  double maxX;   ///< Highest X.
  double minY;   ///< Lowest Y.
  double maxY;   ///< Highest Y.
};

/** What a fixed positioning move is (06 §4). */
enum class FixedMoveKind : uint8_t {
  Probe,  ///< The move to the probing location.
  Zero    ///< The return to X0 Y0.
};

/** A fixed positioning move (fixed-moves.ts): a G0 a transform keeps where it is. */
struct FixedMove {
  uint32_t line;       ///< 0-based line.
  FixedMoveKind kind;  ///< Probe or zero.
};

/** One value of a stock comment. */
struct StockValue {
  uint16_t start;          ///< Byte position of the value in the line.
  uint16_t end;            ///< One past its last byte.
  uint8_t fractionDigits;  ///< Digits after the point.
  Str text;                ///< The value's text, in the arena.
};

/** A valid stock comment (ops/stock.ts StockComment). */
struct StockComment {
  uint32_t line;  ///< 0-based line.
  /** XMIN, XMAX, YMIN, YMAX, ZMIN, ZMAX, in that order. */
  StockValue values[6];
};

/** A half-open range of 0-based line indices. */
struct LineRange {
  uint32_t start;  ///< First line.
  uint32_t end;    ///< One past the last line.
};

/** A tool change block (ops/structure.ts ToolChangeBlock). Line numbers are -1 where TS has -1. */
struct ToolChangeBlock {
  /** The tool, as TS's Number() of the M6 line's digits. */
  double tool;
  Str name;                 ///< The block's name, e.g. "TC3: 1/4in endmill".
  LineRange range;          ///< From its first leading line to its Finish line.
  bool off;                 ///< A spare, commented out with ;OFF.
  int32_t beginToolchange;  ///< The (Begin toolchange) line, or -1.
  int32_t m6;               ///< The M6 line.
  LineRange steps;          ///< The probe steps between (Begin toolchange) and the M6.
  /** From its (MSG Tool change: LABEL, …) step; empty with hasLabel false when TS has null. */
  Str label;
  bool hasLabel;  ///< Whether label is set.
};

/** An operation with its fixtures and tool change (ops/structure.ts Unit). */
struct Unit {
  uint32_t index;             ///< Its position in the document's order.
  Str name;                   ///< The operation's name.
  bool hasTool;               ///< False where TS has a null tool.
  double tool;                ///< The tool it runs with.
  bool enabled;               ///< Not commented out.
  const LineRange* fixtures;  ///< Its fixture blocks.
  uint32_t fixtureCount;      ///< How many.
  LineRange body;             ///< Its leading lines and the operation block.
  uint32_t beginLine;         ///< The operation's Begin line.
  /** Index into Structure::toolChanges of the live tool change before it, or -1. */
  int32_t toolChange;
  const int32_t* spares;  ///< Indices into Structure::toolChanges of the spares kept before it.
  uint32_t spareCount;    ///< How many.
};

/** Whether a file has an operation structure. */
enum class StructureKind : uint8_t {
  Ok,      ///< The structure is filled.
  None,    ///< No operations at all.
  Invalid  ///< Operations, but not ones the editor can rearrange: see Structure::reason.
};

/** ops/structure.ts OperationStructure, plus toolLabels(). */
struct Structure {
  StructureKind kind;  ///< Ok, None or Invalid.
  /** For None / Invalid: the reason (file text included). */
  Str reason;
  /** The reason's 1-based line (0: the whole file). */
  uint32_t reasonLine;
  uint32_t preambleEnd;                ///< Lines [0, preambleEnd) are the preamble.
  uint32_t postambleStart;             ///< Lines [postambleStart, n) are the postamble.
  const Unit* units;                   ///< The operations, in order.
  uint32_t unitCount;                  ///< How many.
  const ToolChangeBlock* toolChanges;  ///< Every tool change block, live or spare, in order.
  uint32_t toolChangeCount;            ///< How many.
  bool hasProbe;                       ///< The preamble has exactly one probe line.
  uint32_t probeLine;                  ///< Its 0-based line.
  Str probeLabel;                      ///< Its label (the tool to insert).
  uint32_t probeLabelStart;            ///< Byte position of the label in the line.
  uint32_t probeLabelEnd;              ///< One past its end.
  /** toolLabels(): the tools, in the TS Map's insertion order. */
  const double* labelTools;
  const Str* labels;    ///< The label of each of labelTools.
  uint32_t labelCount;  ///< How many.
};

/** What analyse() fills (15 §5.2). */
struct Analysis {
  Status status;  ///< Ok, or why the analysis stopped.
  /** For Refused: the 04 §2 message. */
  const char* refusedMessage;
  /** For LineTooLong: the 0-based line. */
  uint32_t longLine;
  bool utf8Bom;        ///< The file starts with a UTF-8 BOM (kept, and skipped).
  uint8_t precision;   ///< P (04 §9): fixed-point values are integers scaled by 10^P.
  Units units;         ///< Units at the first XY motion; else the first G20/G21; else mm.
  bool unitsAssumed;   ///< No G20/G21 before the first XY motion (or none at all).
  bool unitsMixed;     ///< XY motion happens under both G20 and G21.
  uint32_t lineCount;  ///< Lines in the file.
  /** Exact extents in the primary units (TS extents). */
  Extents extents;
  /** Floats in the primary units (TS displayExtents). */
  Extents displayExtents;
  /** Extents without the fixed positioning moves (TS jobExtents). */
  Extents jobExtents;
  const FixedMove* fixedMoves;        ///< The fixed positioning moves, in line order.
  uint32_t fixedMoveCount;            ///< How many.
  int32_t firstXYMotion;              ///< The first line with XY motion, or -1.
  const StockComment* stockComments;  ///< Every valid stock comment, in line order.
  uint32_t stockCommentCount;         ///< How many.
  /** The last valid stock comment's XY box at the precision (TS stockBox), when it fits. */
  Extents stockBox;
  Structure structure;  ///< The operation structure.
  uint32_t parseFlags;  ///< Every line's parse flags, ORed.
  uint32_t simFlags;    ///< Every line's simulation flags, ORed.
  /** Some motion line had an unknown position and gave no segment. */
  bool segmentsSkipped;
  /** Bytes of the arena the call used: size the arena for a job from this (15 §4.2). */
  size_t arenaUsed;
};

/** How analyse() reads. */
struct AnalysisOptions {
  /** Longest line accepted, in bytes (15 §4.2); at most 65,535. */
  uint32_t maxLineLength = 1024;
  /** Called every kCheckpointLines lines; may be null. */
  Progress progress = nullptr;
  /** Passed to progress. */
  void* progressContext = nullptr;
};

/**
 * Reads the file in two passes (15 §5.2, §6.2) and fills `result`. Everything `result` points
 * to lives in `arena`. `sink` may be null. Makes no heap allocation.
 */
Status analyse(LineReader& reader, const AnalysisOptions& options, Arena arena, AnalysisSink* sink, Analysis& result);

}  // namespace gcode
