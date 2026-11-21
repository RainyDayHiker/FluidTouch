# 06 — Testing and spikes

## 1. Test levels

| Level | What | Where |
|-------|------|-------|
| Library | Proven equal to the TS core in CNC-Tools' CI; here, its device suite on the device for each release ([04 §2](04-gcode-library.md#2-the-device-test-suite)); unit tests for segment packing | Test firmware on the device; PC |
| Services | MachineService and JobService against the fake FluidNC, scripted | Simulator, headless |
| Screens | Each screen driven in the simulator with `--script`, screenshots compared by eye at review | Simulator |
| Device | Checklist per milestone on the CrowPanel and the LowRider | By hand |

### Fake FluidNC additions (`sim/tools/fake_fluidnc.py`)

- `$SD/Run`: plays a file from the served folder: moves the reported position along the
  path at feed rate, reports `SD:percent,name`, echoes `(MSG ...)` as `[MSG:...]`.
- `M0`: enters Hold until `~`.
- `G38.2`: sets the probe pin, emits a `[PRB:...]` line.
- Soft reset, feed and rapid overrides affecting playback speed.
- HTTP: file download and multipart upload on the same port layout as FluidNC.
- `$sim/drop=5`: closes the connection for 5 s, to test reconnect during a job.

Done so far: job playback (`$SD/Run`, `SD:` progress, `(MSG ...)`, `M0`, `G4`, `G38.2`, feed
hold, reset, overrides, `$sim/speed`, `$sim/probemiss`, `$sim/drop`; details in
`sim/README.md`, checked by `sim/tools/test_fake_fluidnc.py`) and HTTP on the websocket port
(download, listing, multipart upload with size check, create, delete, rename,
`$sim/httprate`, `$sim/httpblock`). Lines typed in the fake's own console are passed to every connected client's
session (for the `$sim/...` helpers, since the new UI has no terminal yet), and feed hold
(`!`) behaves as on FluidNC: it ends a jog and is ignored when idle. G-code lines sent
directly queue behind each other, as in FluidNC (Setup's probe sends `G91`, `G38.2` and
`G90` back to back); before M2 a line that arrived while another ran was dropped. `$SD/Delete`
deletes a file from the served folder (M3).

The playback models three things taken from FluidNC's source (`InputFile.cpp`, `GCode.cpp`)
that the Run screen has to cope with. S3 measured them on the controller (2026-11-18,
[S3 result](#s3-result-2026-11-18-jeffrey-on-the-machine-router-off-compared-offline)):
the queue is up to about 40 lines, not 15, and a prompt's text came 50 s before its Hold:

- `SD:` percent is the position *read* in the file, ahead of the cut by the planner's
  queue (modelled as 15 lines), and it stops at lines that wait for motion (`M0`, `G4`,
  `G10`, `G38`).
- The `SD:` field disappears when the last line has been read, while the last moves and
  a final `M0` are still to come. "Job active" therefore cannot be `SD:` alone at the end
  of a job.
- `(MSG ...)` text arrives when its line is read. For `M0 (MSG ...)` that is before the
  machine reaches Hold, so the prompt text is known a few moves before the prompt is due.

## 2. Spikes

S1 and S2 were run on 2026-11-16 (S1 during a job on 2026-11-18), S3 and S4 on 2026-11-18; results are below the
table. S5 has not been run; it is deferred until its answer is needed, before M8 (D32).

| # | Question | How | Decides |
|---|----------|-----|---------|
| S1 | Does FluidNC serve SD files over HTTP, and at what URL and speed? | With the machine on, open `http://<fluidnc address>/sd/<a file on the card>` in a browser on the PC. Note whether it downloads and how long the reference job takes. Then repeat while a job is running. See the notes below | Whether "Controller SD" can be previewed, and whether that is allowed mid-job |
| S2 | How much PSRAM is free, and how fast is drawing? | Settings > "Draw test (spike S2)" in the M1 build; see below | Memory budgets in [02 §6](02-architecture.md#6-memory); whether the progress layer can be redrawn or must be incremental |
| S3 | How does `SD:` percent relate to the line being cut? | Run `specs/spikes/s3_air_cut.gcode` (an air cut, D63) with the position log on; see the S3 procedure below. Compare offline with positions computed from the file | How the Run preview maps progress; whether to snap to the nearest segment to the tool |
| S4 | Is HTTP upload to FluidNC reliable for multi-MB files? | Upload a 5 MB and a 9.5 MB file from the display SD with Job > Use this job (UploadManager); note time and failures. `UploadManager` refuses files over 10 MB and is left so (D63); larger files are not tested. See the S4 result below | G8's 32 MB figure; whether a size confirm (S6) is enough |
| S5 | Is OneDrive sign-in and download practical on the ESP32-S3? | A throwaway sketch doing device sign-in, listing one folder, downloading the reference job; note memory use and time. **The sketch has not been written**; it also needs an app registration with Microsoft (a client ID) before it can sign in | Inbox option B |

### S1 result (2026-11-16, Jeffrey, browser on the PC)

- `http://<address>/sd/<path>` downloads the file. **Controller SD can be previewed.**
- `GET /upload?path=<folder>` returns the folder listing as JSON.
- **During a job** (2026-11-18, Jeffrey): the download works while the machine is cutting,
  with `$HTTP/BlockDuringMotion` **On** (FluidNC's default). The source explains it (below):
  `/sd/...` is served by FluidNC's WebDAV handler, which doesn't check that setting.
- Not done: how long the reference job takes to download, and whether a download during a
  cut affects the motion (nothing was noticed; not measured).

### S1: what FluidNC's source says

Read from `WebUIServer.cpp` on FluidNC's main branch (2026-11-16; the first two points
corrected 2026-11-18 from `WebUIServer.cpp` and `WebDAV.cpp`). The result above confirms
them. The fake FluidNC serves `/sd/...` at any time by default, which matches;
`$sim/httpblock=1` makes it refuse downloads with 503 while moving, for testing a failed fetch.

- `/sd/...` and `/flash/...` are served by FluidNC's WebDAV handlers (`WebDAV("/sd",
  SDVolume)`), registered before the not-found handler. A GET downloads the file.
- `HTTP/BlockDuringMotion` (On by default) is checked only when serving the web UI's own
  files from flash (answered with a 503 page, "Cannot load WebUI while GCode Program is
  Running") and for commands sent over HTTP (`/command`). The WebDAV handler doesn't check
  it, so a Controller SD file can be fetched during a job. FluidNC's comments give the
  reason for the setting: serving files costs CPU time that motion may need.
- `GET /upload?path=<folder>` returns the folder as JSON (`files` with `name`, `size`, -1
  for folders; `path`, `total`, `used`, `occupation`, `status`), and takes
  `action=createdir|delete|deletedir|rename`. This is an alternative to `$Files/ListGcode`
  that also gives exact sizes, which S6 (confirm the size after upload) can use.
- An upload whose size field does not match what arrived is discarded by FluidNC.

### S3: what it needs, and what the reference job does

S3 **drives the machine**: it runs a job from FluidNC's SD card while the display records
time, `SD:` percent and position. It needs:

- The recording: the position log (M3, D62), switched on in Settings > Tools.
- The job: `specs/spikes/s3_air_cut.gcode` (D63), below.

#### Position log

While it is on, every job is recorded to the display SD, from Run's start (or a job started
elsewhere being seen) to its end (finished or aborted). Macros are not jobs and are not
recorded.

- File: `/logs/<job name without extension>-NNN.csv`, NNN the first unused number from
  001; the folder is made when missing.
- One row per status report (every 250 ms), none while the connection is down (the gap
  shows in `ms`):

  ```
  ms,state,percent,wx,wy,wz,feed,message
  0,Run,0.00,0.000,0.000,5.000,0,
  250,Run,0.41,1.204,0.000,5.000,1200,"Operation 1 of 2: Pocket"
  ```

  `ms` since the start of the recording; `state` as FluidNC names it; `percent` the `SD:`
  field (empty when there is none); work position; feed rate; `message` only when it
  changed since the row before, quoted.
- Rows are kept in RAM and written every 2 s or 4 KB, and at the end. A missing card or a
  failed write ends that recording; Settings > Tools shows why.
- The switch is the `lrt` key `pos_log` (Off by default), saved with the other settings.

#### The S3 job

`specs/spikes/s3_air_cut.gcode`, made by `specs/spikes/make_spike_jobs.py` (run it again
after changing the script). 884 lines, about 5.5 minutes at the programmed feeds plus the
prompts. It answers the question without the reference job's probe step, and is short
enough to repeat:

| Operation | What | Why |
|-----------|------|-----|
| Start | `G10 L20 P0 X0 Y0 Z0`, up to Z 10, `M0` | Zero where the tool is; a prompt before anything moves |
| 1 Circles | Four 40 mm circles, three passes each (Z 6, 4, 2), full-circle `G2`, F800; then `G4 P1.5` | Arcs; a dwell (Idle with `SD:` still there) |
| 2 Zigzag | 60 x 55 mm of straight lines 2.5 mm apart at Z 2, F1200; then `M0` | Long lines; a prompt halfway |
| 3 Rounded zigzag | Straight lines joined by 2.5 mm half circles (`G2`/`G3`) at Z 3, F1000 | Mixed short arcs and lines |
| 4 Circle of short lines | A 50 mm circle as 720 `G1` lines of 0.22 mm at Z 1, F1500 | Many short lines: the percent should race ahead of the tool here |
| 5 Drifting circles | Half circles, `G2` only, at Z 1.5, F900. Each pair makes a circle 1.5 mm wider than the last, all touching X 144, so they drift right to X 174.5. Meant as a spiral (a slip in the script, kept as run; its message still says "Spiral") | Arcs of changing radius |
| End | Up to Z 10, back to X0 Y0, `M0`, `M2` | Final prompt |

Every move stays at Z 1 or above the start point and inside X 0..175, Y 0..115 of it (arc
bulges included). Each operation starts with `(MSG Operation n of 5: ...)`, so the log's
message column marks when FluidNC *read* each one, which can be compared with the position
at that moment. Played on the fake FluidNC on 2026-11-17: the message for operation 4 was
logged while the tool was still in operation 3.

#### S3 procedure (on the machine, router off)

1. Copy `s3_air_cut.gcode` to the controller's SD (Job > Display SD > Use this job uploads
   it, or FluidNC's web UI).
2. Settings > Tools: Position log On, then leave Settings so it is saved.
3. Jog to a start point with at least 175 x 115 mm clear in +X and +Y and the tool at
   least 15 mm above anything; the job zeroes there.
4. Run it from Run, Continue at each prompt. Settings > Tools then shows the log's name
   and rows (`/logs/s3_air_cut-NNN.csv` on the display SD).
5. Bring the CSV back (display SD to the PC) for the offline comparison.

### S3 result (2026-11-18, Jeffrey on the machine, router off; compared offline)

The log `s3_air_cut-001.csv`: 1,322 rows over 346 s, every message there, no write error.
Compared with the moves in the file by `specs/spikes/s3_compare.py` (`python
specs/spikes/s3_compare.py specs/spikes/s3_air_cut.gcode <log.csv>`), which finds for each
row the move the tool is on. Every row lies within 1 mm of a move of the file.

- **Percent is exact.** It is the byte offset at the end of the last line read, divided by
  the file size, to two decimals (in this 17,807-byte file 0.01% is about 2 bytes, shorter
  than any line). Percent gives the last line read exactly.
- **FluidNC stops reading at `M0`**: the percent stays at the end of the `M0` line until
  Continue.
- **The tool runs behind the line read**, by the planner's queue:

  | Operation | Lines behind (median / max) | Seconds before the tool reached a line it had read (median / max) |
  |-----------|-----------------------------|-------------------------------------------------------------------|
  | 1 Circles (full `G2`) | 0 / 5 | 1.8 / 13.1 |
  | 2 Zigzag (60 mm lines) | 24 / 32 | 32 / 52 |
  | 3 Rounded zigzag | 1 / 33 | 3.5 / 5.9 |
  | 4 Circle of 720 short lines | 42 / 43 | 0.3 / 7.2 |
  | 5 Drifting circles (`G2`) | 1 / 3 | 1.0 / 4.2 |

  So about 40 lines at most when the lines are short. Long straight lines are fewer lines
  but much more time (up to 52 s). Arcs take few lines, because FluidNC cuts each arc into
  many planner blocks. The fake FluidNC models this as 15 lines.
- **Messages arrive when their line is read.** "Halfway - Continue" (`M0`) arrived at
  163 s, with the tool 31 lines back in the zigzag; Hold came at 213 s, **50 s later**. The
  final prompt arrived 3 s before its Hold. "Operation 5" arrived while the tool was 18
  lines from the end of operation 4.
- **Percent is not time.** Operation 4 is 75% of the file's bytes and 14 s of the run. At
  half the run's time the percent was 10.4; at 90% of the time it was 97. Run's time left
  (elapsed x (100 - percent) / percent, [03 §4](03-screens.md)) would have shown 23 to 33
  minutes during the first four minutes of a six-minute job, then 49 s with 76 s to go.
- **Reports**: FluidNC sent one every 250 ms while moving: on moves at constant feed, the
  rows are equal distances apart. During a hold nothing changes and nothing is sent (5.6 s
  at the first prompt). The display's `ms` is up to about 160 ms late on some rows: a
  400 ms gap followed by a 160 ms one, about every second in operation 1, less often later.
  No report was missed.

What this means for the parts S3 decides (Q22 open; D64, D65):

- The Run preview cannot use the percent as the tool's place. It is an upper bound: the
  tool is on a line at or before the one read, and here never more than 45 lines before.
  Matching the position to the moves between the last match and the line read found the
  tool's move in every row here. FluidNC can also report the executing line itself
  (`Ln:`, if `use_line_numbers` is on and the lines carry N words; read from FluidNC's
  `Report.cpp`, not tried). Which to use is Q22, left for M5.
- Time left from the percent is unusable for files whose lines differ in length. It stays
  until M5 replaces it with an estimate from the file (D64).
- The current-operation text and Run's large message (D51) change when a line is read, up
  to 50 s before the tool gets there; from M5 they follow the tool (D65).

### The reference job

What `Spoilboard_DogHoles.gcode` does, read from the file (2026-11-16), for setting the
machine up if it is used:

| Step | Lines | Effect |
|------|-------|--------|
| Zero | `G10 L20 P0 X0 Y0 Z0` | Wherever the tool is when the job starts becomes X0 Y0 Z0 |
| Move | `G0 X10 Y10` | To the probing spot, at the current height |
| Prompt | `M0 (MSG Insert T2 1/8in flat endmill, attach probe)` | Waits for cycle start |
| Probe | `G38.2 Z-110 F200 P0.5` | Moves **down up to 110 mm** until the probe makes contact; the `P0.5` word then makes the contact point Z 0.5 (the plate's thickness), as I read it, not checked on the machine. With no probe attached it keeps going until something stops it, or alarms at the end of the travel |
| Lift | `G1 Z15` | 15 mm above the probed zero |
| Prompt | `M0 (MSG Remove probe and start router)` | Waits for cycle start |
| Spindle | `M3 S12000` | Switches the router on if FluidNC controls it |
| Cut | 45 holes, each a rough and a finish operation | Travel X 0 to 459 mm, Y 0 to 824 mm from the start point; **down to Z -19.5**, 0.5 mm below the 19 mm stock; clearance moves at Z 4.95; feeds 480 to 1200 mm/min, rapids 2400 |
| End | `M5`, `$HZ`, `G0 X0 Y0`, `M0 (MSG Job complete - stop router)`, `M2` | Router off, **homes Z**, returns to the start point, final prompt |

About 14,200 moves and 63 m of cutting; roughly an hour at the programmed feeds, before
acceleration.

For an air cut, the probe step decides everything: Z zero ends up where the probe makes
contact, and the job then goes 19.5 mm below that. So the probe has to be triggered at a
height with at least 20 mm of free space underneath over the whole 460 x 825 mm area, and
the router left off or unplugged.

The fake FluidNC does not model two things this job uses: the `P` word on `G38.2` (it
ignores it) and `$HZ` inside a file (it skips `$` lines).

### S4 result (2026-11-18, Jeffrey)

Uploaded from the display SD with Job > Use this job:

| File | Time | Rate |
|------|------|------|
| `s4_5mb.gcode` (5,000,000 bytes) | 40 s | 125 KB/s |
| `s4_9_5mb.gcode` (9,500,000 bytes) | 65 s | 146 KB/s |

No errors. `UploadManager` refuses files over 10 MB, and that stays (D66): larger files
go to the controller's SD another way. The number of runs per file, and the distance to the
router, were not noted.

## Device test suite runs

The G-code library's device suite (D69, [04 §2](04-gcode-library.md#2-the-device-test-suite))
is run on the device for every library release before the display uses it. One row per
run; the `perf` figures below the table.

| Date | Library version | Groups | Result |
|------|-----------------|--------|--------|
| 2026-11-21 | 0.2.0-build.84 (C2, `applyChecked()`) | all | **Device (Jeffrey, display SD at 20 MHz):** passed 162, failed 0, skipped 0 in 6:47 |
| 2026-11-21 | 0.2.0-build.84 (C2, `applyChecked()`) | all | PC: passed 162, failed 0, skipped 0 (MinGW GCC 16.2, no warnings), and from Settings > Tools in the simulator |
| 2026-11-20 | 0.1.0-build.70 (C2) | all | **Device (Jeffrey, display SD at 20 MHz):** passed 162, failed 0, skipped 0 in 6:03 |
| 2026-11-20 | 0.1.0-build.70 (C2) | all (float, parse, self, analyse, transform, perf) | PC: passed 162, failed 0, skipped 0, with CNC-Tools' host program built by MinGW GCC 16.2 (no warnings), and again from Settings > Tools in the simulator |
| 2026-11-19 | 0.0.1-build.64 (C1) | all (float, parse, self, analyse, perf) | PC: passed 57, failed 0, skipped 0, with CNC-Tools' host program built by MinGW GCC 16.2 (a compiler CNC-Tools' CI doesn't use), and again from Settings > Tools in the simulator |
| 2026-11-19 | 0.0.1-build.64 (C1) | all | **Device, display SD at 20 MHz (D74):** passed 57, failed 0, skipped 0 in 0:58 |
| 2026-11-19 | 0.0.1-build.64 (C1) | all | **Device (Jeffrey, CrowPanel Advance):** passed 57, failed 0, skipped 0 in 1:51. The first attempt rebooted after `float` (the task watchdog: the test task never yielded); fixed by yielding every 50 ms in the SD reader |

`perf` on the device, 0.0.1-build.64 (times include the 50 ms yield, about 2%):

| Job | Size | Lines | Read through the SD reader | `analyse()` (two passes) | Arena (32-bit) | On the PC (MSVC, CNC-Tools' arena.md) |
|-----|------|-------|----------------------------|--------------------------|----------------|----------------------------------------|
| `Spoilboard_DogHoles.gcode` | 789 KB | 14,189 | 2.15 s (about 360 KB/s) | 8.4 s | 69,104 B | 10.7 ms, 85,784 B |
| `CNC_EscapeWheel.gcode` | 157 KB | 2,993 | 0.44 s | 1.65 s | 11,072 B | 2.3 ms |
| 5 MB perf fixture | 5.0 MB | 204,387 | 14.7 s (about 340 KB/s) | 44.5 s | 2,304 B | 77 ms |

SD write: 256 KB in about 785 ms (about 330 KB/s). The device is roughly 600 to 800 times slower
than the PC here. Reading is about half of `analyse()`'s time (it reads the file twice).

Again with the SD reader scanning 16 KB chunks for line ends instead of taking a byte at a time
(device, Jeffrey, 2026-11-19):

| Job | Read | `analyse()` | Arena | Change |
|-----|------|-------------|-------|--------|
| `Spoilboard_DogHoles.gcode` | 2.05 s (about 380 KB/s) | 7.76 s | 69,108 B | read -5%, analyse -8% |
| `CNC_EscapeWheel.gcode` | 0.43 s | 1.52 s | 11,068 B | read -3%, analyse -8% |
| 5 MB perf fixture | 14.1 s (about 360 KB/s) | 42.6 s | 2,304 B | read -4%, analyse -4% |

Write unchanged (783 ms). The per-byte handling wasn't the cost: the display SD runs its SPI
bus at **4 MHz** (`SD.begin(..., 4000000, ...)` in upstream's `UploadManager::init`), which caps
transfers at 500 KB/s, and the card already gives about 380. Reading is still about half of
`analyse()` (two passes of about 2 s for the reference job); the other half, about 3.7 s, is the
library's own work. Hence D74: the display SD at 20 MHz.

At 20 MHz (device, Jeffrey, 2026-11-19; the log says "SD Card mounted at 20 MHz", a 7.4 GB
SDHC card; the suite passed 57 of 57 in 0:58, down from 1:51):

| Job | Read | `analyse()` | Arena | Against 4 MHz with the same reader |
|-----|------|-------------|-------|------------------------------------|
| `Spoilboard_DogHoles.gcode` | 0.73 s (about 1.07 MB/s) | 5.0 s | 69,104 B | read 2.8 times faster, analyse -36% |
| `CNC_EscapeWheel.gcode` | 0.16 s | 0.97 s | 11,072 B | read 2.7 times, analyse -36% |
| 5 MB perf fixture | 5.3 s (about 950 KB/s) | 25.2 s | 2,304 B | read 2.6 times, analyse -41% |

SD write: 256 KB in about 337 ms (about 780 KB/s, was 330). The library's own work is now most
of `analyse()`: about 3.5 s of the reference job's 5.0 s (two reads of 0.73 s each).

0.1.0-build.70 (C2) on the device, display SD at 20 MHz (Jeffrey, 2026-11-20). `apply()`
includes its own `check()`; the output goes to a hash, not the card:

| Job | Read | `analyse()` | Rotate CW `check()` | Rotate CW `apply()` | Arena (analyse / Rotate) |
|-----|------|-------------|---------------------|---------------------|--------------------------|
| `Spoilboard_DogHoles.gcode` | 0.72 s | 4.8 s | 8.0 s | 11.5 s | 69,104 / 140,432 B |
| `CNC_EscapeWheel.gcode` | 0.16 s | 0.96 s | 1.6 s | 2.3 s | 11,072 / 82,392 B |
| 5 MB perf fixture | 5.3 s | 25.1 s | 45.5 s | 64.8 s | 2,304 / 73,632 B |

A transform as the display runs it (check, confirm, apply, then the analysis of the result for
the preview) is therefore about 25 s for the reference job, 5 s for the escape wheel and 135 s
for 5 MB, plus writing the output to the card (about 780 KB/s). `apply()`'s second check is
about 40% of that.

0.2.0-build.84 on the device (Jeffrey, 2026-11-21): `applyChecked()`, the write pass alone:

| Job | `check()` | `applyChecked()` | `apply()` | Check, apply and analysis of the result |
|-----|-----------|------------------|-----------|------------------------------------------|
| `Spoilboard_DogHoles.gcode` | 8.1 s | 3.7 s | 11.8 s | 16.5 s (was 25) |
| `CNC_EscapeWheel.gcode` | 1.6 s | 0.74 s | 2.3 s | 3.2 s |
| 5 MB perf fixture | 45.2 s | 20.8 s | 66.0 s | 90 s (was 135) |

`analyse()` 4.7 s, 0.92 s and 24.4 s, as before. These are the D76 figures with
`applyChecked()` (04 §5).

### S2 procedure

The test screen is built (M1). It was run on the device on 2026-11-16; the result and what
follows from it are at the end of this section. The simulator's memory figures are fixed
placeholders and its times are a PC's.

It uses a synthetic job, not the reference file (there is no parser until M4): 14,189
segments of which 10,689 are arcs, as in the reference job, laid out as 50 pockets cleared
with half circles at six depths, joined by rapids. The segment list is 24 bytes per segment
in PSRAM with arcs kept as arcs; two 480 x 360 RGB565 buffers are allocated (base picture
and progress layer).

On the device: open Settings, press "Draw test (spike S2)", and note every line of the
result (it is also printed on the serial console; the classic screenshot server works too).
Then press LVGL and note its two times; that run may take a while. Zoom switches the
picture between the whole job and 5x. Back frees the buffers.

| Result line | Meaning | Decides |
|-------------|---------|---------|
| Free PSRAM, largest block | With the segment list and both canvas buffers allocated | The budgets in [02 §6](02-architecture.md#6-memory): how much is left for a larger segment list and the outline grids |
| Draw full, Draw 5x | Pixels written straight into the canvas buffer, arcs flattened while drawing | Whether pan and zoom can simply redraw |
| Copy layer + Draw full | Worst case for redrawing the progress layer from the base picture | Whether cut progress is redrawn or drawn incrementally |
| Show on screen | One LVGL refresh of the canvas | Frame cost on top of drawing |
| LVGL: Draw full, Draw 5x | The same job through `lv_draw_line` / `lv_draw_arc` | Whether the preview needs its own rasteriser (expected: yes) |

| Run | Free PSRAM | Largest block | Draw full | Draw 5x | Copy layer | Show | LVGL full | LVGL 5x |
|-----|-----------|---------------|-----------|---------|------------|------|-----------|---------|
| Device, 2026-11-16 | 855 KB | 847 KB | 210 ms | 56 ms | 23.6 ms | 85 ms | 562 ms | 206 ms |

Also reported: free heap 169 KB, generate 15 ms. The free figures are with the test's
1,006 KB allocated (332 KB segment list, 2 x 337 KB canvas), in the new UI, connected to
FluidNC. So about **1.86 MB of PSRAM is free** for the preview in total.

### S2 conclusions

1. **Memory is the constraint, not speed.** The targets that were in
   [02 §6](02-architecture.md#6-memory) (two canvases, a 2 MB segment list, 1 MB of outline
   grids) add up to 3.7 MB; 1.86 MB is there. The reference job fits as tested with
   855 KB to spare. Five times the reference job (G8) does not fit at 24 bytes per
   segment (1.66 MB plus canvases). 02 §6 now carries the measured budget and the
   options (Q21).
2. **The preview draws with its own code, not LVGL's draw functions** (D28): 210 ms
   against 562 ms for the whole job, 56 ms against 206 ms at 5x.
3. **Pan and zoom redraw the picture.** About 0.3 s per step for a job of this size
   (210 ms draw + 85 ms to show). Fine for zoom buttons and stepwise panning; not for
   dragging with a finger. A job five times larger would take about a second per step.
4. **Cut progress is drawn incrementally** (D29). Redrawing the progress layer from
   scratch costs up to 24 + 210 + 85 = about 320 ms, too much to do on every status
   report. Newly cut segments are drawn onto the progress layer as they are passed; a full
   redraw happens only when the view changes.
5. The draw time is dominated by per-arc floating-point trigonometry (10,689 arcs). It
   has not been optimised; there is room if point 3 turns out too slow.

The job was synthetic. Repeat with the reference file when the parser exists (M4/M5).
