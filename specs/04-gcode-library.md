# 04 — G-code library (`lib/gcode`)

The G-code logic is a C++17 library built and tested in the **CNC-Tools** repo, next to the
TypeScript core it reproduces (D67). This repo takes it from a CNC-Tools release (D69) and
builds the UI around it.

- **The spec** is CNC-Tools `gcode-editor/specs/15-cpp-core.md` (revision 5, 2026-11-18;
  on CNC-Tools' branch `spec/cpp-core` until merged). It was started here and reviewed by
  both projects; [outgoing/](outgoing/cnc-tools-15-cpp-core.md) now only points to it.
- **Phases** (15 §2):
  - C0: TypeScript changes that make the core portable (no C++);
  - C1: reading a job (tokenise, simulate, extents, stock, operations, motion segments with
    line and byte offset, a per-line hook);
  - C2: Rotate and Flip ("Flip only" until C4);
  - C3: the Outline Check.

  Each phase is proven bit-identical to the TS core in CNC-Tools' CI, and ends with a
  promoted CNC-Tools release.
- **Move** is not in these phases. It becomes a feature of the G-code Editor first, with its
  own spec, and reaches the display through §3 (D70).

## 1. How it gets here

A release carries one archive, `gcode-core-<version>.zip`, with the library, its docs, the
device test suite and the suite's data (15 §9). The library has the editor's version, e.g.
`0.0.1-build.57`, so a version names one CNC-Tools commit.

- **`tools/gcode_core.py`**, run by a developer with access to CNC-Tools (the GitHub CLI,
  signed in):
  - `fetch <version>` downloads the archive from the release tagged `v<version>` (or takes
    `--zip <file>`), checks every file against its `SHA256SUMS` and the manifest's version,
    and copies `library/` into `lib/gcode/` and the runner's `runner.h` and `runner.cpp` into
    `src/services/gcode_device/` (not `host_main.cpp`, CNC-Tools' host program), each with the
    archive's `manifest.json` and a `SHA256SUMS` for exactly those files;
  - `check` proves both copies match their `SHA256SUMS`, with nothing added (CI runs it);
  - `data <version> <folder>` unpacks the suite's data, checked, for the display SD.

  It does not choose "the newest release": the version to use is written in HANDOFF, normally
  that of a promoted phase release.
- **The suite's `data/`** (several MB: inputs, golden results, vectors) is never committed
  here. It goes onto the SD card used for the device suite, checked against `SHA256SUMS`.
- **Never edit the copies.** CI here (`build-lrt.yml`) runs `tools/gcode_core.py check`. A
  change goes into CNC-Tools (spec, TS, C++) and comes back with a later release.
- `.gitattributes` marks `lib/gcode/**` and `src/services/gcode_device/**` `-text`: this PC
  checks out with `core.autocrlf=true`, which would change the files and break the checksums.
- The build flags the library needs (`-fno-exceptions -fno-rtti -ffp-contract=off`) are in its
  own `library.json` and `CMakeLists.txt`, so PlatformIO and the simulator's CMake
  (`add_subdirectory`) apply them to its sources only.
- CNC-Tools compiles each release with this repo's ESP32 platform (pioarduino
  `platform-espressif32` 55.03.312-1, recorded in the archive's `manifest.json`). **When
  `platformio.ini` changes platform, tell CNC-Tools** so its CI follows.
- Why a copy and not a submodule: this repo is public and CNC-Tools is private, so neither
  this repo's CI nor anyone else could fetch a submodule.

## 2. The device test suite

CNC-Tools' CI proves equality on 64-bit PCs only. On the ESP32-S3, doubles are computed in
software and text-to-number conversion is newlib's, so the same cases are run on the device
(15 §7.4, §9.4):

- It runs from **Settings > Tools > Library tests > Run** in the normal firmware (D73):
  `GcodeTests` (`src/services/gcode_tests.*`) gives the copied runner an SD implementation of
  its file interfaces over `/gcode-tests/` on the display SD, a 1 MB arena from PSRAM (taken
  only while it runs), and a background task, so the screen and the connection keep going.
  It runs every group the release has (`float`, `parse`, `self`, `analyse`, `perf`, then
  `transform` from C2 and `outline` from C3) and writes one line per case to
  `/gcode-tests/results.txt` and the serial port; Tools shows the progress and the summary.
  Not while a job runs.
- **To run it:** `python tools/gcode_core.py data <version> <folder>`, copy the folder's
  contents to `/gcode-tests/` on the display SD, Settings > Tools > Library tests > Run.
- Cost of keeping it in the firmware: about 46 KB of flash, and about 28 KB of internal RAM
  for the runner's static buffers (asked of CNC-Tools: take them from the arena).
- The `perf` group measures time and peak memory per entry point for both reference jobs and
  the 5 MB fixture, and the SD card's read and write rate. **Those figures set the display's
  budgets** (how long an analysis or a Rotate may take, how big a job may be previewed), and
  go back to CNC-Tools for its `arena.md` and the device figure of 15 §12.3.
- Run in M4 on the first release, and **on every later release before the display moves to
  it**. Results are kept with the version tested (in [06](06-test-and-spikes.md)). A failure
  goes back to CNC-Tools as a bug, with `results.txt`.

## 3. What stays in this repo

- The `LineReader` and `ByteWriter` implementations: an SD file on the device, a std file in
  the simulator.
- The arena: where the library's working memory comes from, and how much (02 §6, sized from
  the `perf` results and the library's `arena.md`).
- **Segment packing for the preview**: the library gives full-precision segments (doubles);
  the preview stores them as 8-byte compact segments (D30, [02 §6](02-architecture.md#6-memory)),
  thinned when over budget (keep every Nth within runs of short moves, never dropping rapids
  or the first and last move of an operation; the screen shows "preview simplified").
- **What the per-line hook collects** (15 §5.2): message lines and M0 prompt texts with their
  line and byte offset (for D65 and Run's prompt), and N words if Q22 goes that way. The hook
  gives each line's tokens and state, at the file's final precision.
- Showing a report: the library keeps each issue's line offset, and `reportLineText` reads the
  line back when the list is shown.
- **Text the fonts can't show** (built, D71): LVGL's Montserrat fonts hold ASCII plus a few
  symbols (`°` and `•` among them). The new UI uses them with a fallback font of the same
  face (`src/app/fonts/`, made by `make_extra_fonts.py`) holding Latin-1 (U+00A0..U+00FF),
  `–`, `—`, curly quotes and `…`, which covers the library's summaries (`·`, `–`, `°`).
  Text from outside (file names, FluidNC messages, later the library's names) goes through
  `AppText::forDisplay`: valid UTF-8 is shown as it is; anything else is read as Latin-1, as
  the G-code Editor reads such a file (CNC-Tools 04 §2), with the C1 controls shown as `?`.
  Characters outside these sets show LVGL's placeholder box.
- The outline's cell cap: the algorithm needs 4 bits per cell, so the 1 MB the display can
  spare holds about 2 M cells; its tolerance is coarser than the editor's for large jobs and
  the generated file says so. C3 starts with a design note, reviewed here, that bounds the
  outline's other memory (15 §6.3.1).
- Mapping `SD:` progress and the tool's position to segments on Run (Q22).

## 4. New features from the editor

Features added to the G-code Editor later reach the display this way (CNC-Tools spec 15
§12, D68):

1. **In CNC-Tools:** the feature's spec has a Device section. If it is Portable or Ported,
   it carries a device contract: input and parameters (as data: unit, default, limits,
   messages), preconditions, output, report, choices, memory, passes, cancel. Ask for that
   there, not here, if the display needs a feature that lacks it.
2. **Here, when Jeffrey wants it on the display:** a decision row in 07, the screen in
   [03](03-screens.md) (where it lives, built from the contract and the library's docs: a
   form from the parameter data, the report, confirms for its warnings), and a milestone or
   a place in one in 08.
3. **Port and release:** if the feature is only Portable, it is ported in CNC-Tools first.
   Then the release carrying it is run through the device suite (§2) and copied in (§1).
4. **Build:** the UI only collects parameters, calls the entry point, and shows the
   results. An output file goes to the display SD, and from there to the controller like
   any job (10 MB limit, D66). The device never re-implements G-code logic. A gap in the
   contract is fixed in CNC-Tools' spec, not worked around here.

## 5. Passes on the device

Every job gets one analysis when selected: one or two sequential reads of the card (the
789 KB reference job, `Spoilboard_DogHoles.gcode`, is one of CNC-Tools' reference jobs). A
transform reads the file three times (check; then apply's own check and its rewrite) and
writes a new file on the display SD; the analysis then runs on the result for the preview.
The Outline Check reads the file about four times.
