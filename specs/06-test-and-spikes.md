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

Done so far: lines typed in the fake's own console are passed to every connected client's
session (for the `$sim/...` helpers, since the new UI has no terminal yet), and feed hold
(`!`) behaves as on FluidNC: it ends a jog and is ignored when idle. Until job playback
exists there is no Run state, so Hold is reached with `$sim/state=Hold:0`.

## 2. Spikes

None has been run. S1 to S5 need the machine or the display and are done at the start of M1;
S1 can be done from any browser as soon as the machine is on.

| # | Question | How | Decides |
|---|----------|-----|---------|
| S1 | Does FluidNC serve SD files over HTTP, and at what URL and speed? | With the machine on, open `http://<fluidnc address>/sd/<a file on the card>` in a browser on the PC. Note whether it downloads and how long the reference job takes. Then repeat while a job is running. | Whether "Controller SD" can be previewed, and whether that is allowed mid-job |
| S2 | How much PSRAM is free, and how fast is drawing? | Settings > "Draw test (spike S2)" in the M1 build; see below | Memory budgets in [02 §6](02-architecture.md#6-memory); whether the progress layer can be redrawn or must be incremental |
| S3 | How does `SD:` percent relate to the line being cut? | Run the reference job (air cut is fine) with the M1 build logging time, percent and work position to the display SD. Compare offline with positions computed from the file | How the Run preview maps progress; whether to snap to the nearest segment to the tool |
| S4 | Is HTTP upload to FluidNC reliable for multi-MB files? | Upload a 5 MB and a 20 MB file with the existing UploadManager; note time and failures | G8's 32 MB figure; whether a size confirm (S6) is enough |
| S5 | Is OneDrive sign-in and download practical on the ESP32-S3? | A throwaway sketch doing device sign-in, listing one folder, downloading the reference job; note memory use and time | Inbox option B |

### S2 procedure

The test screen is built (M1) and runs in the simulator; **it has not been run on the
device**, and the simulator's memory figures are fixed placeholders and its times are a PC's.

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
| Device | | | | | | | | |
