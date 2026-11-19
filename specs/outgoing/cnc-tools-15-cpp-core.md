# 15 — C++ core (`packages/core-cpp`)

> For the CNC-Tools repo: `gcode-editor/specs/15-cpp-core.md`. Written in the LowRider Touch
> repo (FluidTouch fork, `specs/outgoing/`), 2026-11-18. The rows to add to the other
> CNC-Tools specs are in §10.

A C++17 library that gives the same results as `@gcode-editor/core` for the parts a small
device needs: reading a job (tokenise, simulate, extents, stock, structure), Rotate, Flip,
Move, and the Outline Check. Its first user is the **LowRider Touch** display: an ESP32-S3
touch panel, an Elecrow CrowPanel Advance, that runs jobs on a FluidNC machine. It previews
a job, rotates or moves it, and writes an outline check, all on the device.

**The TypeScript core stays the reference.** The C++ library has no behaviour of its own: a
change starts in the TS spec and the TS code, and the C++ follows in the same change. CI
proves on every push that both give the same results (§7).

## 1. Why here

- One place for the behaviour, its specs, its fixtures and both implementations. A TS
  change that the C++ doesn't follow fails CI in the same pull request.
- The display's repo copies the library at a pinned commit (§9) and builds UI around it.

## 2. Scope

| Phase | Ports | From (`packages/core/src`) | Needed by the display for |
|-------|-------|----------------------------|---------------------------|
| C1 Read | Tokenizer and vocabulary, line flags, fixed-point numbers, simulator with arcs, stock comments, fixed positioning moves and job extents, operation structure (read side), motion segments | `parse/tokenizer.ts`, `parse/vocabulary.ts`, `model/flags.ts`, `num/fixed.ts`, `sim/simulator.ts`, `sim/arc.ts`, `ops/stock.ts`, `ops/fixed-moves.ts`, `ops/structure.ts` (finding only) | Job info and preview, progress on Run (its next milestone) |
| C2 Transform | Classification with `ROTATE_RULES` and the file-level issues, validation reports, Rotate CW/CCW, Flip H/V, Move | `ops/classify.ts`, `ops/types.ts` (reports), `ops/rotate90.ts`, `ops/flip.ts`, Move (§8) | Rotate, flip and move on the device |
| C3 Outline | Outline Check: validation and the generated file | `ops/outline-check.ts` | Checking a job's outline on the machine |
| C4 Later | Reverse cuts (for flip) | `ops/reverse-cuts.ts` | Not scheduled |

**Not ported:** the document model as a whole-file structure, `DocumentSession` and undo,
text decoding (§4.3), Arrange and enabled forms, the Surfacing generator, the operation
catalog. Features added later, and these, follow §12: each feature spec says whether it is
ported, portable or not, and a portable one carries what the display needs to use it.

## 3. Package

```
gcode-editor/packages/core-cpp/
  CMakeLists.txt     library target gcode_core; tests when built as the top-level project
  library.json       PlatformIO manifest: name gcode-core, srcDir src, includeDir include
  include/gcode/     public headers
  src/               implementation
  test/              runner, unit tests, golden comparison (§7)
  README.md          building, testing, the parity rule
```

- **Self-contained:** nothing outside the folder is referenced by the build. Everything
  generated for tests goes to a gitignored `test/golden/`.
- If pnpm objects to a folder without `package.json` under `packages/`, give it a minimal
  private one whose `test` script runs the C++ tests. Otherwise the folder stays out of the
  pnpm workspace.
- The dependency rule of [02 §2](02-architecture.md#2-repository-layout) applies: `core-cpp`
  includes nothing from the other packages, and nothing includes it.

## 4. Constraints from the device

The display has 8 MB of PSRAM of which about 1.8 MB is free, and the ESP32-S3's FPU is
single precision (doubles are emulated in software). Files are read line by line from an SD
card. Hence:

### 4.1 Language and build

- C++17. Builds warning-free with `-Wall -Wextra` on GCC (host and the Xtensa GCC of
  ESP-IDF 5), Clang and MSVC.
- **No exceptions and no RTTI** (`-fno-exceptions -fno-rtti` in CI). Errors are return
  values.
- Library headers include only the C++ standard library's freestanding-friendly headers
  (`<cstdint>`, `<cstddef>`, `<cstring>`, `<cmath>`, `<climits>`, `<cstdio>` for
  `snprintf` only). No `<iostream>`, `<string>`, `<vector>`, `<map>`, `<functional>`, no
  threads, no files, no OS or Arduino headers. The test runner may use anything.
- Virtual functions are fine (they don't need RTTI).

### 4.2 Memory

- **Streaming:** no whole-file buffer and no per-line storage. Memory grows with the number
  of operations, tool changes, stock comments and reported issues (each capped), never with
  the number of lines.
- **No allocation in the per-line path.** Working memory comes from a caller-supplied arena
  (`Arena{void* base; size_t size;}`), sized up front. Running out of arena is a reported
  error (`OutOfMemory`), never a crash.
- Each entry point documents its arena need: a fixed part, plus per operation, plus, for the
  Outline Check, the grid (§6.3).
- **Line length:** a line longer than the reader's limit (a parameter, default 1024 bytes)
  is a Blocking issue `LineTooLong`, the one rule the TS core doesn't have. No fixture comes
  near it.

### 4.3 Bytes, not text

The C++ library works on bytes. It doesn't decode, and it interprets only ASCII:

- **Refused files:** the same as [04 §2](04-gcode-model.md#2-encoding-and-bytes), with the
  same messages: a UTF-16/32 BOM, or any NUL byte. A UTF-8 BOM is kept and skipped.
- **Invalid UTF-8 or non-ASCII bytes** in comments pass through unchanged. They can't be in
  words, since every recognized token is ASCII. So the TS core's Latin-1 path and this
  library give the same output bytes.
- **Line endings:** `\r\n`, `\n`, `\r`, or none on the last line, split exactly as
  `splitLines` does, kept per line, and written back unchanged.
- **Offsets:** every line carries its 0-based index and its **byte** offset in the file.
  Where the TS core gives a character position within a line, the C++ gives a byte position;
  the golden comparison converts (§7).

### 4.4 Numbers

- Fixed-point values are `int64_t` scaled by 10ᴾ, with P found as
  [04 §9](04-gcode-model.md#9-numbers-precision-and-rounding) says (single pass, rescaling
  as P rises, D38).
- **The TS safe-integer limit is kept:** a value or result with |n| > 2⁵³−1 is
  `PrecisionOverflow`, as in TS, although `int64_t` could hold it. Results must match, not
  merely be correct.
- Arc geometry and every other floating-point step use `double`, with the **same formulas
  in the same order** as the TS code, so that rounding to fixed point (the one rounding
  step, 04 §9) gives the same integers. `ARC_EPSILON` and the other constants are the TS
  values.
- The golden set (§7) is the judge. If a libm difference ever changes a rounded result, the
  case is investigated, not tolerated.

### 4.5 Progress and cancel

Long calls take a progress callback, invoked every `CHECKPOINT_LINES` (10,000) lines with
the line index. Returning false cancels; the call then returns `Cancelled` and has written
nothing that the caller has to keep. This replaces `ApplyContext`'s `AbortSignal`.

## 5. Interfaces

Names and shapes below are the intent. The implementer may refine them, but these
properties must hold: pull-based reading, rewindable for multi-pass work, results through
plain structs and a sink.

```cpp
namespace gcode {

enum class Ending : uint8_t { None, LF, CRLF, CR };

struct Line {
  const char* data;   // without the ending; valid until the next next()
  uint32_t length;
  Ending ending;
  uint32_t index;     // 0-based
  uint64_t offset;    // byte offset of the line's first byte in the file
};

class LineReader {     // implemented by the caller: SD file, memory, std file
 public:
  virtual bool next(Line& out) = 0;     // false at the end or on a read error
  virtual bool rewind() = 0;            // back to the first line, for another pass
  virtual bool failed() const = 0;      // a read error rather than the end
 protected:
  ~LineReader() = default;
};

class ByteWriter {     // the output file of a transform or the outline
 public:
  virtual bool write(const char* data, size_t length) = 0;
 protected:
  ~ByteWriter() = default;
};

}  // namespace gcode
```

### 5.1 Simulator (push, per line)

`Simulator` takes one tokenised line at a time and returns its `LineState`: the same fields
as the TS `LineState` (motion, plane, distance, arc distance, units, WCS, feed mode, before
and after position, owner, arc geometry or the reason it has none, flags). It holds only the
modal state. Classification and the transforms are built on it, as in TS.

### 5.2 Analysis (C1)

`analyse(reader, options, arena, sink, result)` makes as many passes as it needs (at most
two is the aim) and fills:

| Result | As in TS |
|--------|----------|
| Precision P, units summary, line count | `Simulation.precision`, `.units`, `.lineCount` |
| Extents (fixed) and display extents (double) | `.extents`, `.displayExtents` |
| Job extents without fixed moves, and the fixed moves (line, kind) | `jobExtents`, `fixedPositioningMoves` |
| First XY motion line | `.firstXYMotion` |
| Stock comments and the stock box | `findStockComments`, `stockBox` |
| Operation structure: preamble and postamble ranges, units (name, tool, enabled, body range, begin line), tool changes, probe line; or the reason there is none | `findOperationStructure`, `toolLabels` |
| Flags seen anywhere in the file (OR of all lines) | per-line `flags` |

All line ranges also carry byte offsets.

**Segments.** The sink gets one call per motion line in order, for the display's preview
and its progress tracking:

```cpp
struct Segment {
  uint32_t line; uint64_t offset;   // the line it came from
  uint8_t kind;                      // rapid, feed, arc CW, arc CCW; plunge = feed with XY unchanged
  double x0, y0, z0, x1, y1, z1;     // start and end, file units, work coordinates
  double cx, cy;                     // arc centre (arcs only)
  int16_t unit;                      // index in the operation structure, -1 outside any
  uint16_t flags;                    // the line's flags that matter for drawing: unknown position, mixed units, fixed move
};
```

A segment is derived from the line's state, not computed separately: its start is `before`,
its end is `after`, and its centre comes from the arc geometry. Lines with an unknown
position give no segment, and set a flag in the result. Packing segments for drawing is the
display's job, not the library's.

### 5.3 Transforms (C2)

`rotate(reader, writer, dir, options, arena, progress, report)`, `flip(..., axis, ...)` and
`move(..., dx, dy, ...)` each run two passes:

1. Validate and measure: classification with `ROTATE_RULES`, the file-level issues and the
   pivot box. If anything is Blocking, return the report and write nothing.
2. Rewrite every line through the writer: lines without a change are copied byte for byte;
   changed lines get only their X/Y/I/J words (and the stock comment's bounds) replaced, in
   the line's own number style (`styleOf`).

`report` is the TS `ValidationReport`: issues with the same `ruleId`, severity, 1-based
line, message text, and order, capped at `MAX_REPORTED_ISSUES` with the same summary text.
Output also includes the summary message the TS operation shows (06 §7, the flip warning of
12 §3).

### 5.4 Outline Check (C3)

`outlineCheck(reader, writer, sourceName, options, arena, progress, result)` runs
[13](13-export-outline-check.md) §2 to §4, writing the file of 13 §3. Its result carries the
loops (fixed-point vertices at P), the tolerance and the summary text.

## 6. Where C++ may differ from TS

Only these, and each one is named in the code and the tests:

1. **Line length limit**: `LineTooLong` (§4.2).
2. **Passes**: algorithms that use random access in TS (looking ahead to the next XY
   motion in `fixedPositioningMoves`, the backward scans in `findOperationStructure`)
   are rewritten as streaming state machines or as a second pass. Results are identical.
3. **Outline grid size**: the cell cap is a parameter. With the TS cap (25 M cells) the
   result must match TS; the display passes a smaller cap (it can spare about 1 MB, 4 M
   cells at 2 bits each), so its cells are coarser and the file states the larger tolerance
   it gets, as 13 §2 already does for big jobs. The grid is two 1-bit planes taken from the
   arena.
4. **Positions within a line** in bytes rather than characters (§4.3).

## 7. Proving equality

### 7.1 Golden export (TS side)

`tools/export-golden.mjs` runs the TS core over a set of inputs and writes
`packages/core-cpp/test/golden/` (gitignored, regenerated in CI):

- **Inputs:**
  - every file under `packages/core/test/fixtures/` (golden and classification);
  - the round-trip fixtures of 08 §3;
  - small instances from `tools/make-perf-fixture.mjs` with fixed seeds;
  - a set of seeded random files from a generator that mixes recognized and unrecognized
    words, arcs in all forms, unit and plane changes, G91, G92/G10, comments with non-ASCII
    bytes, and all line-ending kinds.
- **For each input** it writes:
  - the analysis (§5.2);
  - every line's `LineState`, one line of JSON per file line, for inputs under 20,000
    lines;
  - for each of Rotate CW, Rotate CCW, Flip H, Flip V (and Move with fixed offsets, once
    it exists): the report, and the output bytes when not blocked;
  - for the Outline Check: the report, the output bytes and the loops.

### 7.2 C++ runner

`core-cpp/test` builds a runner (CMake, CTest) that reads each case and compares:

| What | Rule |
|------|------|
| Output bytes of transforms and outline files | Identical |
| Fixed-point values: extents, positions, stock, loops | Identical integers |
| Doubles (display extents, arc geometry) | Identical to the last bit is expected; if not, within 1e-9 relative, and the case is listed in the run's output |
| Reports and summaries | Same rule ids, severities, lines, messages and order |
| Per-line states | Every field, every line; the first mismatch is printed with the line's text |
| Segments | Consistent with the per-line states (start = before, end = after, centre = arc centre), and counted against the TS motion lines |

Plus C++-only unit tests: the reader (endings, BOM, refusals, rewind), the arena running
out, cancellation, `LineTooLong`, and a counting `operator new` that fails the test if
anything allocates per line.

### 7.3 CI

A job in `.github/workflows/gcode-editor.yml`:

1. Installs pnpm and Node and runs `node tools/export-golden.mjs`.
2. Configures and builds `packages/core-cpp` with `-fno-exceptions -fno-rtti -Werror`.
3. Runs CTest.
4. Compiles the library for the ESP32-S3 (e.g. `pio ci` with `library.json`), so code that
   only builds on the host fails here and not in the display's repo.

The existing `check` job is unchanged. A TS change that alters results makes this job fail
until the C++ follows, in the same pull request.

## 8. Move (new operation, needed before C2 is complete)

Move shifts the job by an exact (dx, dy) in the file's units. Specify it as a TS operation
first, as its own spec (next free number), implement it in TS, then port it:

- Shifts every absolute XY the job cuts at: X/Y words on motion lines, and the stock
  comment's XY bounds.
- Fixed positioning moves (the probe location, the return to X0 Y0) stay as written, as
  for Rotate (D47). I/J are relative and stay.
- Blocks on the same rules as Rotate.
- A result with a negative coordinate: a **warning** or a block. The display's draft
  assumes a warning (its open question Q20); confirm with Jeffrey when Move is specified.
- dx and dy are fixed-point at the document's P; a value with more fraction digits than P
  raises P, as a longer word in the file would.

## 9. How the display uses it (for information)

The display's repo copies `packages/core-cpp` into its `lib/gcode/` with a script, at a
named CNC-Tools commit, and never edits the copy. That repo is public, and copying keeps it
buildable without access to this one. Its CI checks that the copy is unchanged. A change it
needs comes back here first. Both repos are MIT licensed.

So, for this package:

- Tag or name a commit when a phase is done (e.g. `core-cpp-c1`).
- The README states the phase reached and any known difference.

## 10. Changes to other CNC-Tools specs

| Spec | Change |
|------|--------|
| README | Row 15 "C++ core", and a line in the key decisions: a C++17 port for the LowRider Touch display, checked against TS in CI |
| 02 §2 | `packages/core-cpp/` in the layout; the dependency rule covers it |
| 05 | Every operation (and export or generator) spec has a **Device** section (§12.1); the portability rules of §12.2 apply to features marked Portable or Ported |
| 08 §1, §3 | Build: CMake and a C++ compiler for `core-cpp` (developers only). Test strategy: a row for golden parity (§7) and one for the ESP32-S3 compile |
| 09 | Decision: "A C++17 port of the core (read side, Rotate, Flip, Move, Outline Check) lives in `packages/core-cpp` for the LowRider Touch display. The TS core stays the reference; CI proves on every push that the C++ gives the same bytes, numbers and reports." Alternatives: the port in the display's repo with a vendored golden set (it drifts unnoticed); a shared C++ core used by both (rewrites the desktop app's core). Rationale: one owner for behaviour; a TS change can't leave the C++ behind |
| 10 | A milestone per phase (C1 to C3); C1 doesn't depend on any app milestone; C2's Move needs the Move spec and TS implementation first. A later ported feature is part of its own milestone (§12.1), not a new C phase |
| 06, 11, 12, 13, 14 | A Device section each, when next changed or when the display asks for the feature (§12.4) |

## 11. Done when

| Phase | Exit criteria |
|-------|---------------|
| C1 | Every golden case's analysis and per-line states match. The reference job `Spoilboard_DogHoles.gcode` (789 KB) analyses on the host in one call within a fixed arena, and its arena need is stated in the README. The ESP32-S3 compile passes |
| C2 | Every golden transform case matches byte for byte, blocked cases with identical reports. Move is specified, implemented in TS, and in the golden set |
| C3 | Every outline case matches with the TS cell cap; with a 4 M-cell cap the reference job's outline is within the stated tolerance of the TS outline, with the same number of loops |

## 12. New features and the device

The phases above cover what exists today. Features added to the editor later follow this
section, so that each one is either built to be ported or says why not, and a ported one
comes with everything the display needs to use it.

### 12.1 Every feature spec has a Device section

Every new feature spec (an operation, an export, a generator, an analysis), and every change
to an existing one, has a section **Device** that states one of:

| Status | Meaning |
|--------|---------|
| **Ported** (phase Cn) | Implemented in `core-cpp` and in the golden set, in the same change as the TS or in the named phase |
| **Portable** | The TS follows the rules of §12.2 and the spec has the contract of §12.3; nobody has ported it yet |
| **Not portable** | Why (e.g. needs the whole document in memory, needs the viewer), and what would make it portable |

The status is decided with Jeffrey when the feature is specified. A feature spec without
it is not finished. A change to a Ported feature changes the C++ in the same pull request
(CI enforces that, §7.3).

### 12.2 Rules for portable TS code

So that a port is translation, not redesign:

1. **In `core`**, with no UI, viewer or platform code in the logic. The UI only collects
   parameters and shows results.
2. **Passes over the lines in order.** Looking ahead is bounded (a fixed number of lines or
   until a stated event) or done as an extra pass. No random access to `doc.lines` or
   per-line arrays without saying in the spec how it streams.
3. **Memory grows with the job's structure** (operations, tool changes, issues), not with its
   lines. Anything larger (a grid, a buffer) has a size cap as a parameter and says what
   happens at the cap, as the Outline Check's cell cap does.
4. **Numbers:** values written to a file are fixed-point (04 §9); floating point goes through
   the shared helpers, and rounding to fixed point happens in one stated place.
5. **Bytes:** only ASCII is interpreted; output is deterministic (no time, random numbers or
   locale), and lines not changed are copied byte for byte.
6. **Parameters as data:** each parameter has a name, unit, default, limits and validation
   messages exported from `core` as data, as `SURFACING_FIELDS`, `SURFACING_LIMITS` and
   `surfacingErrors` do, so a UI can build its form without knowing the feature.
7. **Results as data:** reports (rule id, severity, line, message), the summary text, and the
   output file's suggested name. Messages are plain sentences with values filled in, no
   markup.
8. **Cancel and progress** at `CHECKPOINT_LINES`.
9. **Fixtures** under `packages/core/test/fixtures/`, and the feature listed in
   `tools/export-golden.mjs`, so its golden cases exist before anyone ports it.

### 12.3 The device contract

A Portable or Ported feature's spec states, for each entry point, enough to build its screen
on the display without reading the TS:

| Item | What to state |
|------|---------------|
| Input | A file, parameters, or both; which parameters (by the data of §12.2 rule 6) |
| Preconditions | What the input must be (e.g. passes Rotate's rules), and what blocks |
| Output | A new file and its name, or results only; what in the job changes, so the preview knows to refresh |
| Report | Rule ids and messages that can occur; which are warnings the user should confirm |
| Choices | Any decision the user makes beyond the parameters (e.g. "flip with reverse cuts") |
| Memory | The arena it needs: fixed, per operation, per cap unit |
| Passes and time | Number of passes over the file; time on the host for the 5 MB perf fixture |
| Cancel | What is left behind when cancelled (nothing, or a partial file the caller deletes) |
| C++ entry point | Name and parameters, once Ported |

### 12.4 Features today

| Feature | Spec | Status |
|---------|------|--------|
| Read: tokenise, simulate, extents, stock, structure | 04, 06 §4, 11 | Ported in C1 |
| Rotate 90° CW/CCW | 06 | Ported in C2 |
| Flip H/V | 12 | Ported in C2 (reverse cuts: C4) |
| Move | to be written (§8) | Ported in C2 |
| Outline Check | 13 | Ported in C3 |
| Arrange operations | 11 | Not assessed; not wanted on the display for now |
| New Surfacing Job | 14 | Not assessed. It takes parameters and writes a file, with parameters already exported as data, so it looks Portable; to be confirmed when its Device section is written |
| Session, undo | 05 | Not portable, not needed: the display keeps the previous file instead |

Each existing spec gets its Device section when it is next changed, or when the display
asks for the feature.

### 12.5 Checklist for a new feature

- [ ] Device status decided and written (§12.1)
- [ ] If Portable or Ported: the TS follows §12.2, and the spec has the contract of §12.3
- [ ] Fixtures added and the feature in `export-golden.mjs`
- [ ] If Ported: C++ in the same pull request, golden cases pass, ESP32-S3 compile passes,
      `core-cpp/README.md` lists the new entry point
