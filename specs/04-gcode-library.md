# 04 — G-code library (`lib/gcode`)

A C++17 library that reproduces the parts of CNC-Tools `@gcode-editor/core` the display
needs. The behaviour is defined by the CNC-Tools specs; this file covers what differs on the
device: streaming, memory, and how equality is proven.

## 1. Constraints

- No Arduino, LVGL, network or filesystem includes. I/O goes through two small interfaces
  (`LineReader`, `LineWriter`) implemented by the firmware (SD file), the simulator and the
  test runner (std files, memory).
- No whole-file buffers. State is the modal state plus per-operation accumulators.
- No exceptions, no RTTI; allocation only at set-up or through a caller-supplied arena.
- Coordinates in transforms use 64-bit scaled integers, as `num/fixed.ts` does, so output
  text is exact.

## 2. Modules

| Module | From (CNC-Tools) | Notes |
|--------|------------------|-------|
| `tokenizer` | `parse/tokenizer.ts`, `vocabulary.ts` | Per line; preserves the original text spans for byte-identical rewrite |
| `fixed` | `num/fixed.ts` | Parse, format with the line's own style, round half away from zero |
| `simulator` | `sim/simulator.ts`, `arc.ts` | Push one line, get the new modal state and zero or one motion segment |
| `analysis` | `ops/stock.ts`, `fixed-moves.ts`, `structure.ts` (read side) | Extents, stock box, tool list, operation list with byte offsets, fixed positioning moves |
| `classify` | `ops/classify.ts`, `ROTATE_RULES` | Same rule ids and messages |
| `transform` | `ops/rotate90.ts`, `flip.ts` | Rotate CW/CCW, flip H/V as one parameterised XY transform |
| `move` | new, CNC-Tools spec to be written (14) | Translate by exact X/Y |
| `outline` | `ops/outline-check.ts` | Grid trace, snap to path, simplify, emit file |
| `segments` | new | Compact segment list for drawing |

Not ported: document model and session/undo, encoding detection (files are treated as
bytes; only ASCII structure is interpreted), arrange operations, reverse cuts (later).

## 3. Passes

Every job gets one **analysis pass** when selected: tokenise and simulate each line; collect
extents, stock, tools, operations, warnings, and the segment list. A 789 KB file is one
sequential read.

A **transform** is two passes over the working copy:

1. Validate and measure (classification, pivot box). Stops with a report if anything blocks.
2. Rewrite: copy each line, replacing only the X/Y/I/J words (and the stock comment), to a
   new file.

Then the analysis pass runs on the result to refresh the preview.

## 4. Segment list

24 bytes per motion: kind (rapid, feed, arc CW, arc CCW, plunge), start and end XY as
float32, Z at end as int16 in 0.01 mm, arc centre as float32 pair, operation index, and the
byte offset of the line. Arcs stay arcs and are flattened when drawn, at the zoom in use.

Over budget ([02 §6](02-architecture.md#6-memory)): keep every Nth segment within runs of
short moves, never dropping rapids or the first/last move of an operation; the screen shows
"preview simplified".

## 5. Outline

Same steps as CNC-Tools spec 13 §2, with the cell size chosen so the grid is at most 4 M
cells (0.05 mm where that fits; about 0.34 mm for the 520 x 864 mm reference job). The
generated file follows spec 13 §3 exactly (header comments, `G10 L20 P0 X0 Y0` without Z,
G0 only, no Z words, back to start, `M2`), and states its tolerance in the header.

## 6. Move (new operation)

Shifts every XY coordinate the job cuts at by (dx, dy): absolute X/Y words on motion lines
and the stock comment's XY bounds. Fixed positioning moves (the probe location, the return
to X0 Y0) stay as written, as for Rotate. I/J are relative and unchanged. Blocks on the same
conditions as Rotate, plus any result with a negative coordinate **Q20**. To be specified in
CNC-Tools first and implemented in both.

## 7. Proving equality with the TypeScript core

- CNC-Tools gains `tools/export-golden.mjs`: for each fixture and each operation it writes
  the input, the operation, the expected output bytes, expected extents, the validation
  report, and the outline loops.
- This repo vendors the exported set under `lib/gcode/test/golden/` with the CNC-Tools
  commit it came from.
- `lib/gcode/test/` builds a PC runner (CMake, same toolchain as the simulator) that must
  match every case: bytes for transforms, exact numbers for extents, same rule ids for
  reports. Outlines: every C++ loop vertex within the stated tolerance of the TypeScript
  loop, and the same number of loops.
- CI runs the runner on every push. A behaviour change starts in the CNC-Tools spec.
