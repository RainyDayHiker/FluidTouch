# 02 — Architecture

## 1. Layers

```
src/app/        screens, tabs, status bar               new   depends on services
src/services/   machine, job, files, macros, settings   new   depends on lib/gcode, platform
lib/gcode/      tokenizer, simulator, ops, outline      new   plain C++17, no Arduino/LVGL
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
| `ui/upload_manager.*` | Used as is by `FileService`. A download function is added next to it (new file). |
| `main.cpp` | After the settings auto-import, `setup()` reads the UI mode: `app` starts `LrtApp::begin()` and returns; `classic` registers the classic listener and continues as before. `loop()` hands over to `LrtApp::loop()` when the app is running. Done in M1. |
| `ui/ui_common.cpp` | The System Options dialog is 70 px taller and has one more button, "Restart into new UI (LowRider Touch)". Done in M1. |
| `platformio.ini` | Adds `lib/gcode` and `src/app`, `src/services` to the build. |
| `sim/` | `CMakeLists.txt`: `src/` on the include path (headers of `src/app` and `src/services` live next to their sources and are included as `"app/..."`, `"services/..."`; PlatformIO already has `src/` on the path), `lib/gcode` later. The forced include is named without a directory so a checkout path with a space builds. Fake FluidNC extended ([06](06-test-and-spikes.md)). |

## 3. UI mode

An NVS key (namespace `lrt`, key `ui_mode`, u8: 0 = `app`, 1 = `classic`; missing or any
other value = `app`) selects the UI at boot. Settings > Tools has "Restart into classic UI";
the classic UI's System Options popup (tap the right end of its status bar) has "Restart
into new UI". Only one UI is created per boot, so RAM use is that of one. The classic
settings auto-import from the display SD runs before the switch, in both modes.

Both UIs use the same stored machine connection: the new SettingsService reads and writes
machine slot 0 of the classic `fluidtouch` preferences. The jog steps, jog feeds and probe
values come from the same slot; Setup writes the chosen steps back to it
(D36). Settings that only the new UI has live in their own namespace (`lrt`): `ui_mode`,
the laser offset `laser_x`, `laser_y` (defaults 33.6 and -28.2, D11), and the probe back-off
`probe_back` (default 10 mm, D43).

Every flash access, read or write, briefly stops the RGB panel from reading its frame from
PSRAM, which shows as a flicker (seen on the device, 2026-11-17). SettingsService therefore
reads slot 0 and the `lrt` values once and keeps them, and writes a changed jog step as its
single key 5 s after the last change, instead of classic's `saveMachine()`, which rewrites
every key of every machine (D44). Settings screens that write (M3) should save on an
explicit action, not per press.

## 4. Services

| Service | Owns | Main operations |
|---------|------|-----------------|
| MachineService | Connection, status snapshot, events (state change, message, alarm, hold, probe result) | jog, cancel jog, home, unlock, zero, go to, hold, resume, reset, overrides, run file, send line |
| FileService | File sources and transfers | list(source, path), fetch to cache, upload to controller, delete; progress and cancel |
| JobService | The selected job: source file, cached copy, analysis (extents, stock, tools, operations, segment list), derived files, run state | select, analyse, transform, build outline, commit (upload), start, progress mapping |
| MacroService | Macro slots | list, run, edit |
| SettingsService | Persistent settings, export/import | get/set, backup to display SD |

### Connection (MachineService)

Implemented in M1. The classic UI connects once and asks the user to reconnect; the new UI
keeps trying on its own (S4):

`No machine | WiFi -> Controller -> Online`, back to `Controller` when FluidNC stops
answering and back to `WiFi` when the network goes.

- WiFi: `WiFi.begin()` without blocking; started again after 15 s without a connection.
- Controller: one `FluidNCClient::connect()` per attempt, 5 s between attempts, 10 s
  allowed for the first status report. The Reconnect button starts an attempt at once
  (ignored while one is under way).
- `connect()` blocks while it resolves the name and opens the socket: 10 to 12 s on the
  device with the controller switched off (2026-11-16), which froze the screen. Each
  attempt therefore runs on its own task on core 0. `FluidNCClient` is not thread-safe, so
  while an attempt runs the main loop does not call `FluidNCClient::loop()` or anything
  else on the client; `MachineService::poll()` owns that call.
- The service records whether a file was running when the connection dropped, for the banner.

Only `MachineService` and `SettingsService` exist so far. `SettingsService` reads classic
slot 0 and reports "not configured" or "not a WiFi machine" (D4). File, Job and Macro
services arrive with the screens that need them (M3).

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

`None -> Fetching -> Analysing -> Ready -> (Transforming -> Ready)* -> Uploading -> Selected
-> Running <-> Held -> Finished | Aborted -> Selected`

A job that is already on the controller and untransformed goes from Ready to Selected
without uploading.

## 5. Storage

| What | Where |
|------|-------|
| Working copy of the selected file, transform outputs, outline file | Display SD, `/lrt/work/` (cleared when a new job is selected) |
| Fallback when no SD card is present | PSRAM, files up to 2 MB; larger files can be run but not previewed |
| Segment list for preview | PSRAM |
| Jobs to run | FluidNC SD, as now. Generated files go to `/sd/lrt/` **Q7** |

## 6. Memory

Measured on the device (spike S2, 2026-11-16; [06](06-test-and-spikes.md#s2-procedure)): in
the new UI, connected, about **1.86 MB of PSRAM is free**, in one block of nearly that size.
Internal heap had 169 KB free. The rest of the 8 MB goes to the code (the build runs from
PSRAM, `CONFIG_SPIRAM_XIP_FROM_PSRAM`), LVGL's 1 MB pool, and the display's full-screen
buffers (two 768 KB LVGL draw buffers in `display_driver.cpp`, plus the panel's own).

Budget for the preview, replacing the earlier targets (which added up to 3.7 MB):

| Use | Size | When |
|-----|------|------|
| Preview canvas, RGB565, 480 x 360 | 337 KB | Job and Run screens |
| Second canvas layer for cut progress | 337 KB | Run screen only |
| Segment list, 8 bytes per segment (D30) | What is left: about 1.5 MB on Job, 1.1 MB on Run (about 144,000 segments) | While a job is selected |
| Outline grids | Whatever is free once the canvases are released | Only while tracing |

At 24 bytes per segment (as tested), 1.1 MB holds about 48,000 segments: 3.4 times the
reference job, short of the "at least 5x" in G8. The options were (Q21, decided as A: D30):

| Option | Effect | Cost |
|--------|--------|------|
| A. Compact segments: end point as two 16-bit values scaled to the job's extents (0.02 mm steps for a job 1.3 m long, 0.04 mm for 2.6 m), start point taken from the previous segment, radius 16 bits, flags and depth level 16 bits: 8 bytes | 1.1 MB holds about 144,000 segments, 10 times the reference job | A little more work per segment when drawing; positions rounded to the step, far below a screen pixel |
| B. Keep the segment list in a file on the display SD and draw from it in blocks | No limit from RAM | Every redraw reads the card: slower pan and zoom; no preview without a card |
| C. Free memory elsewhere: one LVGL draw buffer instead of two, or a smaller LVGL pool | Up to about 1 MB more | Edits to upstream's display driver and `lv_conf.h`; may bring back the flicker those buffers were chosen to avoid; affects the classic UI |

**A is chosen (D30)**, with B as the fallback for jobs that still do not fit (preview
"simplified, and says so", G8). C is not planned.

The outline grids (2 x 1 bit per cell) get whatever is free while tracing: with the canvases
released and a reference-sized job loaded that is about 1.5 MB, enough for the 4 M cells
(1 MB) planned. For larger jobs the grid gets coarser.

## 7. Threads

| Context | Runs |
|---------|------|
| Main loop (core 1) | LVGL, FluidNCClient loop, service polling |
| Worker task (core 0), one at a time | fetch, analyse, transform, outline, upload |
| Connect task (core 0), short-lived | one `FluidNCClient::connect()` attempt ([§4](#4-services)) |

Worker results are handed over by a flag the main loop checks; no LVGL calls from the worker.
