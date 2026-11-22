# 11 — Preview and progress (M5)

How a chosen job is fetched, analysed and drawn on Job, and how Run follows the tool.
Milestone M5 ([08](08-milestones.md)); decisions D79 to D82, D28 to D30. The screens are
[03 §3](03-screens.md#3-job) and [03 §4](03-screens.md#4-run); memory and threads are
[02 §6, §7](02-architecture.md#6-memory); the analysis is the library's (`analyse()` with the
outline, D75, D77).

The geometry comes from the library's segments, the same ones the editor's viewer draws, so
the picture can't drift from the editor's top view. The colours, the scale and how the
display follows the tool are the display's.

§9's proposals were accepted by Jeffrey on 2026-11-21 (D82).

## 1. Building order

M5 is built and checked in three parts, each its own branch:

| Part | Delivers | Checked by |
|------|----------|------------|
| 5a | Working folder, fetch from the Controller SD, the analysis on the worker task with a progress bar and Cancel: `PreviewService` (`src/services/preview_service.*`), `Fetching -> Analysing -> Ready`. Built 2026-11-21 | Simulator with the fake FluidNC; device: the reference job fetched and analysed, times against D75 |
| 5b | Segment list (§3), the Job preview (§4), the info line with the time estimate (§5) | Simulator; device: the reference jobs look like the editor's top view |
| 5c | Run: progress layer, tool marker, the tool's line (§6), op N of M, the message following the tool, time left from the estimate (§7) | Fake FluidNC playback; device: a real cut |

## 2. Fetch and the working folder

- **Controller SD file:** downloaded when chosen on Job (D72) with HTTP GET
  `http://<address>/sd/<path>` into `/lrt/work/<name>`, on the worker task, with a progress
  bar from the bytes received and Cancel. The working folder is cleared first (02 §5). The
  analysis runs on the copy. Run matches this copy to the running job by name and size.
- **Display SD file:** analysed where it is, not copied (P1, D82). The working folder holds
  only fetched files, and from M6 the transforms' outputs.
- **Chosen on Job during a cut:** not downloaded then (D72); Job says "Downloaded for the
  preview when the job ends." and the download starts when it ends.
- **During a cut, no copy:** Run's preview area offers the download with the confirm of
  [05 §2](05-file-workflow.md#2-model). "Not now" keeps the pre-M5 view.
- A fetch that fails (controller offline, the card full, a short download) deletes the
  partial file and shows the reason on Job's preview area, with Retry ("The file is no longer
  on the controller's SD." for a 404). Cancel leaves "Preview cancelled." with Retry.
- **Choosing another file** while one is downloaded or analysed cancels it and starts the new
  one. The library tests (Settings > Tools) take the preview's arena back while they run: the
  preview shows Retry afterwards.
- **No display SD card:** a Controller SD file is chosen and run as before, with no preview,
  and the preview area says "Insert a display SD card to preview this job." (P2, D82).

## 3. The segment list

The analysis sink receives every segment (`AnalysisSink::segment`, CNC-Tools 15 §5.2) and
packs it into 8-byte records in PSRAM, one per segment, laid out as D30 says:

| Record | Bytes |
|--------|-------|
| Move (one per segment) | end X and end Y as signed 16-bit steps; for an arc its radius as a signed 16-bit step count (negative for more than 180°), else 0; 16 bits of meta: kind (rapid, feed, arc CW, arc CCW, 2 bits), fixed move (1), Z as a depth level (8 bits), the line's distance from the previous record's line (5 bits, 0 to 30) |
| Extra, rare | a full circle's centre (start and end are the same point, so the radius doesn't place it); the absolute line when the distance is over 30; F when the feed changes (mm/min as a float; G93 inverse time flagged) |

- **XY step:** starts at 0.01 mm, so ±327 mm. A value that doesn't fit doubles the step and
  repacks the records so far (a pass over the list, a few times at most per job). The final
  step is below a screen pixel at Fit for any job up to 10 m. A move's start is the previous
  record's end.
- **Z level:** signed 8-bit steps from the first Z, starting at 0.1 mm and doubled the same
  way (the reference job, Z -19 to 15, ends at 0.2 mm). Enough for the depth colours and for
  matching the tool's Z (§6).
- **Checkpoints:** every 256 records, the record index, its line, its byte offset and its
  absolute position, so Run can search by line or offset (§6) without decoding from the start.
- **Units:** inch files are converted to mm when packed.
- **Capacity:** about 1.0 MB on Job, 0.6 MB on Run (02 §6): about 75,000 records on Run, over
  5 times the reference job's segments (G8), as D30 planned. A job with more records keeps those that fit,
  and the preview says "Preview shows the first 63% of the job." D30's file fallback is not
  built in M5.
- **Line numbers (D79):** when the line hook (`AnalysisSink::line`) sees an N word, its value
  and line go into an N table in the same memory, 8 bytes per numbered line. The table is only
  used if the N words increase through the file; otherwise it is dropped and position
  matching is used (§6).

## 4. The Job preview

- **Canvas:** the preview area, about 784 x 220, RGB565, drawn by the display's own
  rasteriser into the buffer (D28; `draw_test.cpp` is the starting point).
- **What is drawn:** rapids thin and dim; feeds and arcs 2 px, coloured by their end Z, from
  the job's top cut depth (light) to its deepest (dark), in 8 levels (P4, D82);
  plunges as a dot; the stock box (`Analysis::stockBox`), dashed; job zero as a cross; the
  probe point (`fixedMoves`, kind Probe) as a ring. Fixed moves are drawn as rapids.
- **Order:** file order, so a later pass is drawn over an earlier one.
- **Fit** fits the job's extents (`jobExtents`), with the stock box if there is one, and
  keeps the aspect ratio. **Tap** zooms in 2x centred on the tap, up to 16x. The Fit button
  moves into the preview's corner (its row is M6's). A redraw takes about 0.3 s for the
  reference job (S2, D28).
- **While analysing:** the preview area shows the progress bar, Cancel and the file line's
  details. The preview, the info line, the extents and the first tool on Run's start panel
  fill in when the analysis finishes (D75).
- **Refused or failed analysis:** the library's message (`refusedMessage`) or the status, in
  the preview area. The job can still be used and run.

## 5. The info line and the time estimate

`520 x 864 mm  Z -19..15  T2  90 ops  ~2h10`

| Part | From |
|------|------|
| Size | `jobExtents`, in the file's units |
| Z range | lowest and highest Z of the segments |
| Tools | `structure.labelTools` ("T1 T2"); left out when the file has none |
| Ops | enabled units in `structure`; left out when `structure.kind` isn't Ok |
| Time | the estimate below, "~" and hours:minutes |
| Warnings | P3 (D82) |

**The time estimate (D80)** is computed by the display from the segment list, not by the
library: an exception to "no G-code knowledge here", chosen so M5 doesn't wait for CNC-Tools.

- A feed or arc: its length (arcs by their angle and radius) at the line's F. G93: 1/F
  minutes. G95, or no F yet: the axis rates below, and the estimate is marked incomplete
  ("~2h10+").
- Every move, rapids included, is capped by the machine's axis rates: the time is at least
  each axis's distance over its `max_rate_mm_per_min`, read once from the controller's config
  (`$/axes/x/max_rate_mm_per_min` and likewise for Y and Z) on connect. Without them a rapid
  is taken at 5,000 mm/min and the estimate is marked "+".
- `G4 P` dwells add their time; prompts (`M0`) add nothing, being waits for the user.
- No acceleration: the estimate is short on jobs of many tiny moves. The S3 log gives a
  first check of how short; the reference job's real time on the device gives a second.

## 6. Run: following the tool (D79)

Each status report gives the work position and the `SD:` percent (how far FluidNC has read).
The tool's record is found as follows, at every report:

1. **`Ln:` present** (FluidNC with `use_line_numbers: true`, and the file's N table in use):
   the N is looked up in the table, giving the line, and the line's first record is found
   from the checkpoints. The record within that line is the one nearest the position. Exact.
2. **Otherwise, position matching (S3):** the window runs from the last matched record to the
   record at the `SD:` read position (checkpoints by byte offset). The match is the first
   record in the window whose move passes within 1 mm of the position in XY and 0.5 mm (or one Z level) in Z.
   No match keeps the last one. A path that retraces itself picks the earliest, which stays
   right because the window only moves forward.
3. A position outside every move (a jog in a hold, a probe) keeps the last match.

Then:

- **Progress layer:** the records from the last drawn one up to the match are drawn bright onto
  Run's second canvas, incrementally (D29). The marker (a cross) is at the reported position,
  and follows the tool when zoomed in.
- **Op N of M:** the unit whose line range holds the matched line, from `structure`. Left out
  when the file has no structure.
- **The message follows the tool (D65):** a `[MSG:...]` is stored with the `SD:` read position
  at which it arrived, and shown when the match passes that position. A prompt (`M0` HOLD) is
  shown when it happens, since FluidNC holds when the tool gets there.

## 7. Time left

From the match: the estimate of the records still to come (§5), with the feed override applied
to feeds and arcs and the rapid override to rapids, plus nothing for the prompts still to come.
After 2 minutes of cutting it is scaled by how far the actual time has differed from the
estimate so far. This replaces the percent formula (D64). Without a segment list (no card, too
large, or no copy) the percent formula stays.

## 8. Memory and threads

As 02 §6: one canvas on Job, two on Run, the 512 KB arena while a job is selected, and the
segment list (with the N table) in what is left. The fetch and the analysis run on the worker
task, one at a time, yielding (02 §7). Packing a segment costs a few divisions; the repacking
at a doubled step runs on the worker task too.

## 9. Proposals accepted (D82)

| # | Proposal | Why |
|---|----------|-----|
| P1 | A display SD file is analysed where it is, not copied to `/lrt/work/` first (02 §5 says a working copy) | A copy costs about 1 s per MB and gains nothing until a transform writes one (M6) |
| P2 | No PSRAM fallback without a card (02 §5 says files up to 2 MB in PSRAM): no preview without a display SD card | 02 §6 leaves about 1 MB on Job for segments after the canvas and the arena; 2 MB of file doesn't fit beside them |
| P3 | The info line's warnings wait for M6, where `check()`'s report gives them with the library's texts. M5 shows only the preview's own: "Units assumed: mm", "Mixed units", "Some moves had an unknown position and aren't drawn", "Preview shows the first N%" | The analysis gives flags, not texts; the transform report is where the editor's warnings come from |
| P4 | Depth colours (Q15): 8 levels from light to dark over the job's cut Z range; rapids grey | Q15's draft; easy to change once seen on the device |

## 10. Testing

- **Simulator:** fetch from the fake FluidNC (its HTTP `/sd/`), Cancel, a failed fetch, the
  analysis progress and Cancel; the reference jobs drawn, Fit and zoom; a step doubling (a job
  over 327 mm); the info line and estimate; Run with fake playback: progress, marker, op N of M,
  messages following the tool, time left. The fake FluidNC gains `Ln:` for files with N words
  and its config's axis rates.
- **Host test:** the record packing and unpacking round trip against the library's segments for
  the golden set, within half a step.
- **Device:** the reference job fetched, analysed and drawn, its times against D75; drawing
  times against S2; a real cut followed on Run, with and without `use_line_numbers`; the
  estimate against the real time.
