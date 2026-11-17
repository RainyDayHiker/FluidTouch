# 06 — Testing and spikes

## 1. Test levels

| Level | What | Where |
|-------|------|-------|
| Library | `lib/gcode` against golden cases from CNC-Tools ([04 §7](04-gcode-library.md#7-proving-equality-with-the-typescript-core)); unit tests for segments and move | PC runner, CI |
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
`G90` back to back); before M2 a line that arrived while another ran was dropped.

The playback models three things taken from FluidNC's source (`InputFile.cpp`, `GCode.cpp`)
that the Run screen has to cope with; none has been measured on the controller, which is
what spike S3 is for:

- `SD:` percent is the position *read* in the file, ahead of the cut by the planner's
  queue (modelled as 15 lines), and it stops at lines that wait for motion (`M0`, `G4`,
  `G10`, `G38`).
- The `SD:` field disappears when the last line has been read, while the last moves and
  a final `M0` are still to come. "Job active" therefore cannot be `SD:` alone at the end
  of a job.
- `(MSG ...)` text arrives when its line is read. For `M0 (MSG ...)` that is before the
  machine reaches Hold, so the prompt text is known a few moves before the prompt is due.

## 2. Spikes

S1 (partly) and S2 were run on 2026-11-16; results are below the table. S3 to S5 have not
been run and are deferred until their answers are needed (D32): S3 and S4 with M3, when a
job is first started and a file first uploaded from the new UI; S5 before M8.

| # | Question | How | Decides |
|---|----------|-----|---------|
| S1 | Does FluidNC serve SD files over HTTP, and at what URL and speed? | With the machine on, open `http://<fluidnc address>/sd/<a file on the card>` in a browser on the PC. Note whether it downloads and how long the reference job takes. Then repeat while a job is running. See the notes below | Whether "Controller SD" can be previewed, and whether that is allowed mid-job |
| S2 | How much PSRAM is free, and how fast is drawing? | Settings > "Draw test (spike S2)" in the M1 build; see below | Memory budgets in [02 §6](02-architecture.md#6-memory); whether the progress layer can be redrawn or must be incremental |
| S3 | How does `SD:` percent relate to the line being cut? | Run the reference job (air cut is fine) with the M1 build logging time, percent and work position to the display SD. Compare offline with positions computed from the file | How the Run preview maps progress; whether to snap to the nearest segment to the tool |
| S4 | Is HTTP upload to FluidNC reliable for multi-MB files? | Upload a 5 MB and a 20 MB file with the existing UploadManager; note time and failures | G8's 32 MB figure; whether a size confirm (S6) is enough |
| S5 | Is OneDrive sign-in and download practical on the ESP32-S3? | A throwaway sketch doing device sign-in, listing one folder, downloading the reference job; note memory use and time. **The sketch has not been written**; it also needs an app registration with Microsoft (a client ID) before it can sign in | Inbox option B |

### S1 result (2026-11-16, Jeffrey, browser on the PC)

- `http://<address>/sd/<path>` downloads the file. **Controller SD can be previewed.**
- `GET /upload?path=<folder>` returns the folder listing as JSON.
- Not done yet: the same download while a job is running, the value of
  `$HTTP/BlockDuringMotion`, and how long the reference job takes to download.

### S1: what FluidNC's source says

Read from `WebUIServer.cpp` on FluidNC's main branch (2026-11-16). The first and third
points are confirmed by the result above. The fake FluidNC follows this.

- There is no handler for `/sd/...`. A GET for any unregistered path goes to the not-found
  handler, which tries to send the file with that path as a download; a path that starts
  with `/sd/` is a file on the SD card. So `http://<address>/sd/<path>` should work.
- With the setting `HTTP/BlockDuringMotion` on, such a request is answered with **503**
  while the machine is moving. S1 should note that setting's value (`$HTTP/BlockDuringMotion`)
  and what the browser gets during a job. If it is on, the Controller SD file can only be
  fetched for preview before the job starts, which is when it is needed.
- `GET /upload?path=<folder>` returns the folder as JSON (`files` with `name`, `size`, -1
  for folders; `path`, `total`, `used`, `occupation`, `status`), and takes
  `action=createdir|delete|deletedir|rename`. This is an alternative to `$Files/ListGcode`
  that also gives exact sizes, which S6 (confirm the size after upload) can use.
- An upload whose size field does not match what arrived is discarded by FluidNC.

### S3: what it needs, and what the reference job does

S3 **drives the machine**: it runs a job from FluidNC's SD card while the display records
time, `SD:` percent and position. Two things are not there yet:

- The recording. Nothing in the M1 build writes a log to the display SD. It has to be added
  (a few lines per status report to a file) before S3 can be run.
- A decision on which job. S3 does not need the reference job; any job with arcs, a few
  operations and an `M0` answers the question, and a short one is easier to repeat.

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
