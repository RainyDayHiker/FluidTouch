# 04 — G-code library (`lib/gcode`)

The G-code logic is a C++17 library built and tested in the **CNC-Tools** repo, next to the
TypeScript core it reproduces (D67). This repo copies it and builds the UI around it.

- **The spec** is CNC-Tools `gcode-editor/specs/15-cpp-core.md`. Its first version was
  written here: [outgoing/cnc-tools-15-cpp-core.md](outgoing/cnc-tools-15-cpp-core.md).
  Once it is in CNC-Tools, that copy is the one that counts.
- **What it provides**, in phases:
  - C1: reading a job (tokenise, simulate, extents, stock, operations, motion segments
    with line index and byte offset);
  - C2: Rotate, Flip and Move;
  - C3: the Outline Check.

  Each phase is proven equal to the TS core by a golden comparison in CNC-Tools' CI.

## 1. How it gets here

- `lib/gcode/` is a copy of `gcode-editor/packages/core-cpp/` at a named CNC-Tools commit,
  made by a script (to be written with the first copy). The commit is recorded in
  `lib/gcode/SOURCE`.
- **Never edit `lib/gcode/` here.** A change goes into CNC-Tools (spec, TS, C++) and comes
  back with the next copy. CI here checks that the copy matches its recorded commit's
  checksum.
- A copy is used rather than a git submodule because this repo is public and CNC-Tools is
  private: a submodule would stop this repo's CI, and anyone else, from building.
- PlatformIO finds it as a library in `lib/` (it has a `library.json`); the simulator adds
  it with `add_subdirectory`. Its own tests run in CNC-Tools, not here.

## 2. What stays in this repo

- The `LineReader` and `ByteWriter` implementations: an SD file on the device, a std file in
  the simulator.
- The arena: where the library's working memory comes from, and how much (02 §6).
- **Segment packing for the preview**: the library gives full-precision segments; the
  preview stores them as 8-byte compact segments (D30, [02 §6](02-architecture.md#6-memory)),
  thinned when over budget (keep every Nth within runs of short moves, never dropping rapids
  or the first and last move of an operation; the screen shows "preview simplified").
- The outline's cell cap: about 4 M cells (1 MB) on the device, so its tolerance is coarser
  than the editor's for large jobs and the generated file says so.
- Mapping `SD:` progress and the tool's position to segments on Run (Q22).

## 3. New features from the editor

Features added to the G-code Editor later reach the display this way (CNC-Tools spec 15
§12):

1. **In CNC-Tools:** the feature's spec has a Device section. If it is Portable or Ported,
   it carries a device contract: input and parameters (as data: unit, default, limits,
   messages), preconditions, output, report, choices, memory, passes, cancel. Ask for that
   there, not here, if the display needs a feature that lacks it.
2. **Here, when Jeffrey wants it on the display:** a decision row in 07, the screen in
   [03](03-screens.md) (where it lives, built from the contract: a form from the parameter
   data, the report, confirms for its warnings), and a milestone or a place in one in 08.
3. **Port and copy:** if the feature is only Portable, it is ported in CNC-Tools first.
   Then `lib/gcode/` is copied at that commit.
4. **Build:** the UI only collects parameters, calls the entry point, and shows the
   results. An output file goes to the display SD, and from there to the controller like
   any job (10 MB limit, D66). The device never re-implements G-code logic. A gap in the
   contract is fixed in CNC-Tools' spec, not worked around here.

## 4. Passes on the device

Every job gets one analysis when selected: reading the 789 KB reference job is one or two
sequential reads of the card. A transform is a validate pass and a rewrite pass to a new
file on the display SD, then the analysis again for the preview.
