# 11 — Preview and progress (M5)

How a chosen job is fetched, analysed and drawn on Job, and how Run follows the tool.
Milestone M5 ([08](08-milestones.md)); decisions D79 to D84, D28 to D30. The screens are
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
| 5a | Working folder, fetch from the Controller SD, the analysis on the worker task with a progress bar and Cancel: `PreviewService` (`src/services/preview_service.*`), `Fetching -> Analysing -> Ready`. Built 2026-11-21; on the device 2026-11-22: the reference job analysed in 6.5 s | Simulator with the fake FluidNC; device: the reference job fetched and analysed, times against D75 |
| 5b | Segment list (§3, `src/services/segment_list.*`), the Job preview (§4, `src/app/preview_canvas.*`), the info line with the time estimate (§5). Built 2026-11-22 | Simulator; host test of the packing; device: the reference jobs look like the editor's top view |
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
- **During a cut, no preview of the running job:** the working folder's copy when its size is
  the controller's and no other file is chosen, else Run offers the download with the confirm
  of [05 §2](05-file-workflow.md#2-model) (D94: `RunPreview`). When the controller refuses the
  listing, a copy of that name is offered in the confirm beside the download (D98). "Not now" keeps the pre-M5 view
  with the button. A job started elsewhere has no size until the listing (or the analysed
  file) gives it; Run then follows it by name and that size.
- A fetch that fails (controller offline, a write to the display SD failing, a short
  download) deletes the partial file and shows the reason on Job's preview area, with Retry ("The file is no longer
  on the controller's SD." for a 404). A write to the display SD that fails ("Writing to the
  display SD failed.") is first retried once from the start, without a word (D86). Cancel
  leaves "Preview cancelled." with Retry.
- **Choosing another file** while one is downloaded or analysed cancels it and starts the new
  one. The library tests (Settings > Tools) take the preview's arena back while they run: the
  preview shows Retry afterwards.
- **No display SD card:** a Controller SD file is chosen and run as before, with no preview,
  and the preview area says "Insert a display SD card to preview this job." (P2, D82).

## 3. The segment list

The analysis sink receives every segment (`AnalysisSink::segment`, CNC-Tools 15 §5.2), and
the line hook (`AnalysisSink::line`) the line's tokens and state just after it; the move is
packed there into 8-byte records in PSRAM, one per segment, laid out as D30 says with the
changes of D84:

| Record | Bytes |
|--------|-------|
| Move (one per segment) | end X and end Y as signed 16-bit steps from work zero; for an arc the centre's signed distance from the chord's middle (left of the chord positive) in 16-bit steps, else 0; 16 bits of meta: kind (rapid, feed, arc CW, arc CCW, 2 bits), fixed move (1), Z as a depth level (8 bits), the line's distance from the previous move's line (5 bits, 0 to 30) |
| Extra, rare (meta's line distance 31) | the centre of a full circle, or of an arc whose ends round to the same step; the absolute line when the distance is over 30 or goes back; F when the feed changes (mm/min, or 1/min under G93, as a float; the mode flagged: G94, G93, G95 or no F yet); a dwell (G4 P, seconds); a jump: where the next move starts when that isn't the last move's end (G92, G28, a move after an unknown position), not drawn; the next move's N word (D79); padding, so a move and its extras never straddle a checkpoint |

An arc stored by its radius (D30) puts a half circle's centre off by up to √(2·r·step): 0.3 mm
for a 5 mm radius at 0.01 mm. The centre's distance from the chord is exact for a half circle,
and fits the same 16 bits (D84). An arc smaller than a step is stored as a straight move; a
G18/G19 arc, or one the library couldn't interpret, as a straight move to its end.

- **XY step:** starts at 0.01 mm, so ±327 mm. A value that doesn't fit doubles the step and
  repacks the records so far (a pass over the list, a few times at most per job). The final
  step is below a screen pixel at Fit for any job up to 10 m. A move's start is the previous
  record's end.
- **Z level:** signed 8-bit steps from the first Z, starting at 0.1 mm and doubled the same
  way (the reference job, Z -19 to 15, ends at 0.2 mm). Enough for the depth colours and for
  matching the tool's Z (§6).
- **Checkpoints:** every 256 records, from the top of the same memory down (32 bytes each):
  the state before that record (the last move's line, its byte offset and end, the feed, the
  last N word) and the estimate up to there with its feeds' share (§5, §7, filled by the
  estimate), so Run can start decoding at any checkpoint (§6) instead of from the start.
- **Units:** inch files are converted to mm when packed.
- **Capacity (D83, D89):** one block, sized when a job is first chosen: the largest free
  PSRAM block less 128 KB of headroom (Run's canvas is allocated at boot), at most 640 KB (about 80,000
  records; almost 6 times the reference job's 13,662). On Job and Run alike. A job with more records
  keeps those that fit, and the preview says "Preview shows the first 63% of the job" (by
  file offset). D30's file fallback is not built in M5.
- **Line numbers (D79):** when the line hook sees an N word on a line that moves, its value goes
  into an extra record before the move (8 bytes per numbered move). The N words are only used
  if they increase through the file: from the first one that doesn't, no more are stored and
  position matching is used (§6).
- **Measured (host, 2026-11-22):** the reference job packs into 13,662 records (107 KB, step
  0.04 mm, Z step 0.2 mm), every end within 0.035 mm of the library's; the escape wheel 2,953
  records at 0.01 mm.

## 4. The Job preview

- **Layout (D83):** the preview area is 784 x 204 under the file line (content y 80 to 284);
  the info line and the warnings under it are left of Use this job (y 296 to 352). With no
  picture the area says why: "Choose a job to see its preview.", the download or analysis
  with its bar and Cancel, the upload, the reasons of §2 with Retry, "No XY moves to draw."
- **Canvas:** RGB565, drawn by the display's own rasteriser into the buffer (D28; from
  `draw_test.cpp`), allocated once when the Job screen is built (313 KB).
- **What is drawn:** rapids 1 px grey; feeds and arcs 2 px, coloured by their end Z, from the
  top of the cutting moves' Z range (light) to its bottom (dark), in 8 blues (P4, D82); the
  cutting moves are the feeds and arcs that move in XY, so retracts at feed don't stretch the
  range. A plunge (a feed down with XY unchanged) is a dot; a feed up isn't drawn. The stock
  box (`Analysis::stockBox`) dashed; job zero as a cross; the probe point (`fixedMoves`, kind
  Probe) as an orange ring. Fixed moves are drawn as rapids.
- **Order:** file order, so a later pass is drawn over an earlier one.
- **Fit** fits the job's extents (`jobExtents`), with the stock box if there is one, 10 px
  in from the edges, and keeps the aspect ratio. **Double tap** zooms in 2x centred on the
  tap, up to 16x; at 16x it centres. Two taps are a double tap when the second ends within
  400 ms of the first and 40 px from it (not LVGL's own double click, which missed finger taps
  on the device). A single tap does nothing. A press is a drag once it has moved 16 px. **Drag**, only when zoomed in,
  moves the view: the picture already drawn follows the finger, and it is drawn again when the
  finger lifts; at least 20 px of what Fit shows stays in view. Zooming out is Fit only (D85).
  The Fit button is in the preview's top right corner (its row is M6's). A redraw takes 0.2 s
  for the reference job on the device (5b); 3 ms in the simulator.
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
| Time | the estimate below with the allowances for probes, homing and prompts (D101, D102), "~" and hours:minutes |
| Warnings | P3 (D82): on the line under it, in orange |

Lengths are whole mm when they are within 0.05 of one, else to a tenth; an inch file's in
inches to a hundredth. Each part is left out when there's nothing for it (no XY moves, no
structure, no segments). The time is "~2h10" from an hour, "~25 min" below, at least
"~1 min".

"Some moves had an unknown position and aren't drawn" is shown when a move in XY got no
segment from the library (an unknown position, Z included) and something is missing from the
picture: not when only its start was unknown, the first move of most files from wherever the
machine is, nor for a Z move after a probe (whose Z is unknown until the probe).

**The time estimate (D80)** is computed by the display from the segment list, not by the
library: an exception to "no G-code knowledge here", chosen so M5 doesn't wait for CNC-Tools.

- A feed or arc: its length (arcs by their angle and radius) at the line's F. G93: 1/F
  minutes. G95, or no F yet: the axis rates below, and the estimate is marked incomplete
  ("~2h10+").
- Every move, rapids included, is capped by the machine's axis rates: the time is at least
  each axis's distance over its `max_rate_mm_per_min`, read once from the controller's config
  (`$/axes/x/max_rate_mm_per_min` and likewise for Y and Z) on connect. Without them a rapid
  is taken at 5,000 mm/min and the estimate is marked "+". FileService asks for them 2 s after
  each connect, one at a time (the answer is `$/axes/x/max_rate_mm_per_min=8000.000`; an
  `error:` leaves that axis unknown), and keeps them in MachineService. An arc is capped by
  the slower of X and Y over its length. Rates that arrive after the analysis start the
  estimate again (a pass over the list, a few ms).
- `G4 P` dwells add their time.
- **Allowances (D101, D102).** The moves leave out what has no planned length: a probe
  (`G38.x`: it stops where the plate is, and the lift after it starts from an unknown Z) and a
  homing (`$H`, `$HZ`: not a move the library knows). Each gets a fixed allowance, 20 s a
  probe and 10 s a homing (CenterWheel's position log, 2026-11-27: 19 to 21 s each probe and
  its lift, 10 s each `$HZ`). The info line adds them, and 45 s for each prompt (`M0`;
  CenterWheel's 7 took 5.3 min, from 8 s to 2 min each), so its time answers "is there time
  to run this?". Run's time left adds the probes and homings still ahead but not the
  prompts: it stops while one waits (D90). The lines are found as FluidNC reads them
  (JobNotes): comments and anything after `;` left out, letters in either case; a `G38`
  word (any decimal), an `M0` or `M00` word (not `M1`, not `M03`), a line starting `$H`. The
  lines of the first 128 probes and homings are kept for Run; all are counted.
- No acceleration: the estimate is short on jobs of many tiny moves. The S3 log gives a
  first check of how short; the reference job's real time on the device gives a second.

## 6. Run: following the tool (D79)

Each status report gives the work position and the `SD:` percent (how far FluidNC has read).
The tool's record is found as follows, at every report:

1. **`Ln:` present** (FluidNC with `use_line_numbers: true`, and the file's N words
   increasing, D93): `Ln:` is the N word of the move FluidNC is executing (its planner's
   current block; FluidNC sends none for a line without one, nor in a dwell or a hold). The
   checkpoints hold the last N before them, so the move with that N is in the block of the
   last checkpoint below it: at most 256 records read. One line is one move in the list, so
   that move is the match. Exact. A number no move has (a jog in a hold, a line the list has
   no move for) and a move before the last match are ignored, and position matching is
   tried instead; so is a report without `Ln:` (a file with N words on some lines only).
   The log says when the match changes between the two ("RunFollow: following by Ln:").
2. **Otherwise, position matching (S3):** the window runs from the last matched record to the
   record at the `SD:` read position (checkpoints by byte offset). The match is the first
   record in the window whose move passes within 1 mm of the position in XY and 0.5 mm (or one Z level) in Z.
   No match keeps the last one. A path that retraces itself picks the earliest, which stays
   right because the window only moves forward. Where moves lie closer together than the
   tolerances (a small helix, a raster under 1 mm apart) the match trails the tool by those
   moves: the reference job's holes (a 0.64 mm helix, 0.2 mm down per quarter turn) by a
   quarter turn or two, a fine raster by a pass. It never gets ahead.
3. A position outside every move (a jog in a hold, a probe) keeps the last match.

Then:

- **Run's preview (D89, D91):** one canvas, 568 x 344, left of the panel. The job is drawn as on Job (§4), and the moves before the match in one bright green over
  it (rapids a lighter grey), incrementally (D29): each report draws the moves from the last
  drawn one up to the match. A view change (Fit, zoom, drag, following) redraws the whole
  canvas, the job then what is done. Fit, double tap and drag as on
  Job (§4). The marker, a cross at the reported work position, is its own small object over
  the canvas, so moving it draws nothing; hidden when outside the view. **Following:** when
  zoomed in and the marker comes within 15% of an edge (or leaves the view), the view centres
  on it. A drag stops following, so the view stays where the finger put it; Fit or a double
  tap starts it again. Fit is hidden at a prompt (the prompt box covers its corner). On the
  start panel the job is shown with nothing done. Without a preview of this job (no card, the analysis failed, a job started elsewhere,
  a different file) the left side shows the file and the message in large type, as before M5.
- **The tool's job:** Run follows the preview when its file is the running one: the same name
  and size (§2).
- **Op N of M:** the enabled unit whose lines (its body or one of its fixtures, from
  `structure`) hold the matched move's line, numbered among the enabled ones; between
  operations (a tool change) the last one stays. Left out before the first and when the file
  has no structure (D92).
- **The message follows the tool (D65, D92):** the analysis keeps the file's `(MSG ...)`
  comments with their lines, read as FluidNC reads them (a comment holding `MSG`, from its
  fifth character; `;` ends the line), in 16 KB (the reference job's 93 take 4 KB; when full,
  later ones are left out). While Run follows a running job and the controller's last message
  is one of the file's, the status bar shows the file's last message at or before the matched
  line instead; before the first match, at a prompt (`M0` HOLD: FluidNC holds where the tool
  is, so its message is the current one), and for the controller's own messages, the
  controller's. A message is the file's when its text, spaces ignored, matches one kept, also
  shifted by a character (a FluidNC that drops a comment's spaces before cutting the first
  four). This replaces storing each `[MSG]` with the `SD:` position it arrived at, which needed
  a listener event in upstream code and lost messages that arrived together.

## 7. Time left

From the match: the estimate of the records still to come (§5), with the feed override applied
to feeds and arcs and the rapid override to rapids, plus the allowances of the probes and
homings still to come (D101), plus nothing for the prompts still to come.
After 2 minutes of cutting it is scaled by how far the actual time has differed from the
estimate so far. This replaces the percent formula (D64). Without a segment list (no card, too
large, or no copy) the percent formula stays, with D90's averaging.

How it is shown (D91), so it is steady and still corrects itself:

- **Planned time with a correction.** Each new match gives a target: the estimate still to
  come x the correction factor. The factor is actual running time / estimated time of the
  part done; it starts at 1 and moves toward the measured ratio slowly, from 2 minutes of
  cutting on. Both are seconds, unlike the `SD:` percent, which counts bytes.
- **Counted down by the clock.** Between reports the value shown goes down one second per
  second of running time, and stops while the job waits (prompt, pause; D90).
- **No jumps.** A new target moves the shown value toward it a part of the way each second,
  not all at once.

The target counts down from the match but never below the plan after the matched move x the
factor, so a long move that takes longer than planned doesn't run it to zero. Constants
(`TimeLeft`, D92): the factor settles with a 60 s time constant of running time and stays
within 0.5 to 5; the shown value closes on the target with a 20 s time constant. Feeds and
arcs are scaled by the feed override, rapids and dwells by the rapid override; the part done
is summed at the overrides it ran at, so a change of override doesn't spoil the factor. New
axis rates (§5) make the estimate again; the position is taken over in the new numbers.
Not with a truncated list (the plan doesn't reach the end): the percent formula.

**Probes and homing (D101).** Their allowances (§5) still ahead of the matched line are added
to the target as they are, not scaled by the factor nor by the overrides, and counted down
with the rest. The floor after the matched move leaves out the ones between the matched move
and the next planned move (the ones being done now), so they count down too. The factor
learns from cutting only: when the match moves past a probe or homing line, the running time
since the last match and the plan between the two matches are both left out of it. Without
this, the probe at the start of a job whose first operation is short (the Third Wheel: a
2 min pocket) took the factor to about 1.2 and time left climbed from 30 toward 35 minutes.

**The operation's time left (D100)** beside "op N of M": the same count to a nearer point,
the end of the current operation's last move (found once per operation, reading on from the
match until a later operation starts; again with a new estimate). The plan from the match to
there over the overrides, x the same factor, counted down from the match, not below the plan
after the matched move. Not smoothed (it is short, and a jump is at most the error of one
report). Hidden between operations (the matched line in none) and without a planned time
left. CenterWheel replayed (`replay.sh`): within 0.7 min of the truth for every operation,
one minute in and halfway.

The usual alternatives, compared 2026-11-23: extrapolating from percent (D51, D90: noisy,
because bytes aren't time); FigUI's planned time counted down by the clock (steady, but never
corrected: when the estimate runs out it shows nothing); smoothing the rate alone (moving
average, Kalman: hides the noise, doesn't remove it). The constants (how slowly the factor
moves, how far each second closes the gap) are set in step 2 against the reference jobs.

## 8. Memory and threads

As 02 §6: one canvas on Job, two on Run, the 512 KB arena while a job is selected, and the
segment list (with its N words) in what is left. The fetch and the analysis run on the worker
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
  every case of the release's device data (`tools/host_tests/run.sh <cases folder>`): kind,
  line, ends within one step (a repack rounds twice), an arc's middle point within 3 steps
  (for steps under 1 mm: the fuzz cases with kilometre steps only check ends), a reader from
  each checkpoint reading the same moves as one from the start, a job over 327 mm doubling the
  step, a list too small truncating. 121 files, 0 failures (2026-11-22). Run's tracker
  (5c): the tool put at the middle of each move in turn, the match never goes back nor past
  the tool's move; how often it is that very move is printed (100% in 82 files; the reference
  job 46%, its helices; `perf-5mb` 14%, a fine raster). 121 files, 0 failures (2026-11-22).
  Messages (step 2): in line order, each recognised as sent with and without its spaces and
  shifted by one, a controller message not taken for the file's. Time left (step 2), every job
  of a minute or more "run" with each move taking 1.25 x its estimate + 0.05 s, a report every
  0.25 s: the reference job within 1% from a quarter of the way on, steady after the factor
  settles (about 5 minutes in: it shows the plan uncorrected for the first 2 minutes, then
  climbs); the escape wheel within 4 to 6%; with the feed override going to 150% at 40%, 3 to
  13%. Jobs of a few long moves end low (the tool's progress within a move isn't known).
  121 files, 0 failures (2026-11-23).
  `Ln:` (step 3): for each file whose N words increase, each move's N is the one written on
  its line, and a report with that `Ln:` and a position far off finds that very move; an N
  no move has keeps the match. The reference job with an N on every line: all 13,301 moves
  found by `Ln:` (by position 45.9% on the very move). 122 files, 0 failures (2026-11-23).
- **A real cut replayed:** `tools/host_tests/replay.sh <file> [position log] [x,y,z rates]`
  prints the estimate per `(MSG ...)` part of the file and, with a position log from the
  device (Settings > Tools), the time left the display would show every 3 minutes of running
  against the time really left (the overrides taken as 100%). CenterWheel (2026-11-27): the
  estimate 72.6 min against 74.8 min cut, time left within 1.9 min from 15 min on (HANDOFF).
  It also prints the operation's time left (D100) beside the true one, and how far off it was
  per operation a minute in and halfway.
- **Device:** the reference job fetched, analysed and drawn, its times against D75; drawing
  times against S2; a real cut followed on Run, with and without `use_line_numbers`; the
  estimate against the real time.
