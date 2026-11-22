# 02 — Architecture

## 1. Layers

```
src/app/        screens, tabs, status bar               new   depends on services
src/services/   machine, job, files, macros, settings   new   depends on lib/gcode, platform
lib/gcode/      tokenizer, simulator, ops, outline      copy  from a CNC-Tools release (D67, D69); C++17, no Arduino/LVGL
src/services/gcode_device/  the library's device test runner  copy  from the same release (D73)
src/core/       display, touch, power                   kept
src/network/    FluidNCClient, upload                   kept, small edits
src/ui/         classic UI                              kept, untouched where possible
sim/            SDL simulator, fake FluidNC             kept, extended
```

Rules:

- A layer includes only layers below it. `lib/gcode` includes nothing from the project.
- Screens never parse FluidNC text and never call `FluidNCClient` directly.
- Slow work (download, parse, transform, outline, upload) runs in a FreeRTOS task on core 0
  and publishes progress in a small state struct polled from the LVGL loop, as
  `UploadManager` does today. LVGL is only touched from the main loop.

## 2. Changes to kept code

Kept deliberately small, to stay mergeable (G11):

| File | Change |
|------|--------|
| `network/fluidnc_client.*` | The direct calls to `UICommon` (connection popups) and `UITabControlProbe::updateResult` go through `FluidNCClientListener` (one listener, set with `FluidNCClient::setListener`). Events: connection established, connection lost, probe result. Done in M1. The header still includes `ui/machine_config.h` for `MachineConfig`; left as is. |
| `ui/classic_client_listener.*` (new file in upstream's folder) | The classic UI's listener; does what the direct calls did. |
| `ui/upload_manager.*` | Used by `FileService`. `init()` mounts the display SD at 20 MHz, and at upstream's 4 MHz if that fails, and logs which (D74). |
| `main.cpp` | After the settings auto-import, `setup()` reads the UI mode: `app` starts `LrtApp::begin()` and returns; `classic` registers the classic listener and continues as before. `loop()` hands over to `LrtApp::loop()` when the app is running. Done in M1. |
| `ui/ui_common.cpp` | The System Options dialog is 70 px taller and has one more button, "Restart into new UI (LowRider Touch)". Done in M1. |
| `platformio.ini` | Adds `lib/gcode` and `src/app`, `src/services` to the build. |
| `sim/` | `CMakeLists.txt`: `src/` on the include path (headers of `src/app` and `src/services` live next to their sources and are included as `"app/..."`, `"services/..."`; PlatformIO already has `src/` on the path), `lib/gcode` added with `add_subdirectory` and linked (its own flags for its own sources). The forced include is named without a directory so a checkout path with a space builds. Fake FluidNC extended ([06](06-test-and-spikes.md)). |

## 3. UI mode

An NVS key (namespace `lrt`, key `ui_mode`, u8: 0 = `app`, 1 = `classic`; missing or any
other value = `app`) selects the UI at boot. Settings > Tools has "Restart into classic UI";
the classic UI's System Options popup (tap the right end of its status bar) has "Restart
into new UI". Only one UI is created per boot, so RAM use is that of one. The classic
settings auto-import from the display SD runs before the switch, in both modes.

Both UIs use the same stored machine connection: the new SettingsService reads and writes
machine slot 0 of the classic `fluidtouch` preferences. The jog steps, jog feeds and probe
values come from the same slot; the default jog steps too, which Setup starts with but
does not write (D45). Settings that only the new UI has live in their own namespace (`lrt`): `ui_mode`,
the laser offset `laser_x`, `laser_y` (defaults 33.6 and -28.2, D11), and the probe back-off
`probe_back` (default 10 mm, D43).

Every flash access, read or write, briefly stops the RGB panel from reading its frame from
PSRAM, which shows as a flicker (seen on the device, 2026-11-17). SettingsService therefore
reads slot 0 and the `lrt` values once and keeps them, and the working screens never write
(D44, D45). Settings is the one place that saves, on an explicit action, not per press (M3).
Classic's `saveMachine()` rewrites every key of every machine, so the new Settings writes
only the keys that changed, itself, with the classic key names (`m<slot>_ssid`, `m<slot>_jxy_fd`
and so on, `pm_*` and `display_rot`), and then marks classic's machine cache stale. Which
slot is used comes from `lrt` `machine_slot` (D57; 0 when not set). After a save,
`SettingsService::generation()` changes, so screens that copied values (Setup) take them
again when next shown, and `MachineService::useMachine()` reconnects when the slot or the
connection changed.

## 4. Services

| Service | Owns | Main operations |
|---------|------|-----------------|
| MachineService | Connection, status snapshot, events (state change, message, alarm, hold, probe result) | jog, cancel jog, home, unlock, zero, go to, hold, resume, reset, overrides, run file, send line |
| FileService | File sources and transfers | list(source, path), fetch to cache, upload to controller, delete; progress and cancel |
| JobService | The selected job: source file, derived files, run state | select, generate (a new job, [10](10-new-jobs.md)), transform, build outline, commit (upload), start, progress mapping |
| PreviewService | The selected job's working copy and analysis (extents, stock, tools, operations, segment list), the library's arena ([11](11-preview.md)) | load (fetch from the Controller SD, then analyse, on the worker task), cancel, retry; progress |
| Generators | The list of generators and their common interface: texts, fields, messages, plan, write ([10 §7](10-new-jobs.md#7-services)) | list, form data, messages, plan, write |
| MacroService | Macro slots | list, run, edit |
| SettingsService | Persistent settings, export/import | get/set, backup to display SD |

### Connection (MachineService)

Implemented in M1. The classic UI connects once and asks the user to reconnect; the new UI
retries on its own for a while, then waits for Reconnect (S4, D46):

`No machine | WiFi -> Controller -> Online`, back to `Controller` when FluidNC stops
answering and back to `WiFi` when the network goes.

- WiFi: `WiFi.begin()` without blocking; started again after 15 s without a connection.
- Controller: one `FluidNCClient::connect()` per attempt, 10 s allowed for the first
  status report. After a failed attempt or a dropped connection the retries come after 5,
  5, 10 and 10 s; then the service stops and waits for Reconnect (`waitingForReconnect()`).
  A failed attempt takes 10 to 12 s on the device with the controller off, so that is
  about a minute and a half of trying. Reconnect starts an attempt at once and the back-off
  again (ignored while an attempt is under way); so do a successful connection and WiFi
  coming back. WiFi itself is retried without limit.
- `connect()` blocks while it resolves the name and opens the socket: 10 to 12 s on the
  device with the controller switched off (2026-11-16), which froze the screen. Each
  attempt therefore runs on its own task on core 0. `FluidNCClient` is not thread-safe, so
  while an attempt runs the main loop does not call `FluidNCClient::loop()` or anything
  else on the client; `MachineService::poll()` owns that call.
- The service records whether a file was running when the connection dropped, for the banner.

`SettingsService` reads classic slot 0 and reports "not configured" or "not a WiFi machine"
(D4).

`MacroService` (M3) keeps the 12 slots in RAM, read once from the `lrt` key `macros` (one
blob with a version byte) and written back only when the list changed. It runs a macro
(`$SD/Run` or the command line) and follows it: starting, running, held, done or stopped,
with the reason. `JobService` leaves files under `/fluidtouch/macros/` to it (D61).

`PositionLog` (M3) records a running job's status reports to the display SD when Settings
> Tools has it on ([06](06-test-and-spikes.md) S3, D62).

`FileService` (M3) lists a folder on either card, deletes a file, and uploads a display SD
file. Controller listings are `$Files/ListGcode` replies collected through
`FluidNCClient::setMessageCallback` (one request at a time, 6 s timeout); deletes are
`$SD/Delete`, followed by a fresh listing. An upload first lists the uploads folder (is the
name there already?), then runs `UploadManager` (classic, unchanged; it posts to port 80,
creates the folder, and refuses files over 10 MB), then lists the folder again and compares
the size (S6).

`JobService` (M3) holds the chosen file and the run. A job counts as running from START (or
from the `SD:` field, for a job started elsewhere) until FluidNC has been Idle with no `SD:`
field for 1.5 s, which covers the end of a file, where `SD:` goes once the last line is read.
A hold is a prompt unless it was asked for here (Pause or STOP: `MachineService::heldByUser()`).
An alarm, or a reset from Abort, ends it as Aborted. While it runs, `MachineService::jobActive()`
is true even after `SD:` has gone, so Setup stays greyed out to the end (S3).

`MachineService` holds the rules for what may be done in which state (`canHome`, `canZero`,
`canMove`, `canUnlock`, `canReset`; [03 §2](03-screens.md#2-setup) and S3). Screens use them
to grey out buttons and to explain a press they refuse (`busyReason()`), and each action
checks them again before it sends anything. Home XY sends `$HY` from `poll()` once the `$HX`
cycle has ended. Probe Z
sends `G91` and `G38.2`, and puts `G90` back when the result arrives; after a miss FluidNC
is in alarm and would reject it, so it goes out with the next Unlock instead. A reset
cancels it, since a reset restores the modal defaults.

`src/app` uses the classic colour constants (`ui/ui_theme.h`, header only) so both UIs look
alike; nothing else from `src/ui` is included by the new code except `ui/machine_config.h`.

### Job state machine

`None -> (Fetching | Generating) -> Analysing -> Ready -> (Transforming -> Ready)* -> Uploading -> Selected
-> Running <-> Held -> Finished | Aborted -> Selected`

A job that is already on the controller and untransformed goes from Ready to Selected
without uploading.

## 5. Storage

| What | Where |
|------|-------|
| A Controller SD file fetched for the preview, transform outputs, outline file, a generated new job | Display SD, `/lrt/work/` (cleared when a new job is selected). A display SD file is analysed where it is (D82) |
| The last values of each New job form, as typed | Display SD, `/lrt/new-job-values.txt`, written on Create ([10 §6](10-new-jobs.md#6-the-values-kept)) |
| No display SD card | No preview: a Controller SD file can be chosen and run, not previewed (D82) |
| Segment list for preview | PSRAM |
| Jobs to run | FluidNC SD, as now. Uploaded and generated files go to `/sd/fluidtouch/uploads/` (D53) |

## 6. Memory

Measured on the device (spike S2, 2026-11-16; [06](06-test-and-spikes.md#s2-procedure)): in
the new UI, connected, about **1.86 MB of PSRAM is free**, in one block of nearly that size.
Internal heap had 169 KB free. The rest of the 8 MB goes to the code (the build runs from
PSRAM, `CONFIG_SPIRAM_XIP_FROM_PSRAM`), LVGL's 1 MB pool, and the display's full-screen
buffers (two 768 KB LVGL draw buffers in `display_driver.cpp`, plus the panel's own).

Budget for the preview, replacing the earlier targets (which added up to 3.7 MB):

| Use | Size | When |
|-----|------|------|
| Preview canvas, RGB565 (Job: 784 x 204, D83) | 313 KB | Job and Run screens |
| Second canvas layer for cut progress | 337 KB | Run screen only |
| Library arena (analysis with the outline, transforms) | 512 KB, provisional until C3's device `perf` figures (D77) | While a job is selected |
| Segment list, 8 bytes per segment (D30) | Sized for Run (D83): the largest free block less 400 KB for Run's second canvas, at most 640 KB (about 80,000 records, 5.9 times the reference job, G8) | While a job is selected |

At 24 bytes per segment (as tested), 1.1 MB holds about 48,000 segments: 3.4 times the
reference job, short of the "at least 5x" in G8. The options were (Q21, decided as A: D30):

| Option | Effect | Cost |
|--------|--------|------|
| A. Compact segments: end point as two 16-bit values scaled to the job's extents (0.02 mm steps for a job 1.3 m long, 0.04 mm for 2.6 m), start point taken from the previous segment, radius 16 bits, flags and depth level 16 bits: 8 bytes | 1.1 MB holds about 144,000 segments, 10 times the reference job | A little more work per segment when drawing; positions rounded to the step, far below a screen pixel |
| B. Keep the segment list in a file on the display SD and draw from it in blocks | No limit from RAM | Every redraw reads the card: slower pan and zoom; no preview without a card |
| C. Free memory elsewhere: one LVGL draw buffer instead of two, or a smaller LVGL pool | Up to about 1 MB more | Edits to upstream's display driver and `lv_conf.h`; may bring back the flicker those buffers were chosen to avoid; affects the classic UI |

**A is chosen (D30)**, with B as the fallback for jobs that still do not fit (preview
"simplified, and says so", G8). C is not planned.

The Outline Check is built in the same analysis as the preview (D77, CNC-Tools 15 §18):
a grid of at most 1 M cells at 2 bits a cell (250 KB), the same cells and tolerance as the
editor's, taken from the top of the library's arena and given back when the outline is
done. What stays held while the job is selected is the outline's corners and report (about
9 KB for the reference job). The arena needs analyse()'s own peak plus about 400 KB for a
full 2440 × 1220 mm sheet (the reference job's analysis peaks at 69 KB): 512 KB, held while
a job is selected, which leaves the segment list 5.2 times the reference job on Run (G8). A
1 MB arena would leave it 1.6 times. Past the arena only the outline is unavailable, never
the preview. The size is checked against CNC-Tools' `arena.md` and C3's device `perf`
results ([04](04-gcode-library.md)).

## 7. Threads

| Context | Runs |
|---------|------|
| Main loop (core 1) | LVGL, FluidNCClient loop, service polling |
| Worker task (core 0), one at a time | fetch, analyse, transform, outline, upload |
| Connect task (core 0), short-lived | one `FluidNCClient::connect()` attempt ([§4](#4-services)) |

Worker results are handed over by a flag the main loop checks; no LVGL calls from the worker.

**A task doing long work yields regularly** (`delay(1)` at least every 50 ms; the G-code
library's line reader is the natural place). The library and SD reads never block, and a
task on core 0 that doesn't yield starves the idle task until the task watchdog reboots the
device (seen with the library tests, 2026-11-19). The simulator runs tasks as threads and
doesn't catch this; check it in review.
