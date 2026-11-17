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
| `main.cpp` | Registers the classic listener after `FluidNCClient::init()` (M1). Reads the UI mode at boot and starts either the classic UI or the new app. |
| `platformio.ini` | Adds `lib/gcode` and `src/app`, `src/services` to the build. |
| `sim/` | `CMakeLists.txt`: `src/` on the include path (headers of `src/app` and `src/services` live next to their sources and are included as `"app/..."`, `"services/..."`; PlatformIO already has `src/` on the path), `lib/gcode` later. The forced include is named without a directory so a checkout path with a space builds. Fake FluidNC extended ([06](06-test-and-spikes.md)). |

## 3. UI mode

An NVS key selects `app` (default) or `classic` at boot. Settings > Tools has "Restart into
classic UI"; the classic UI's System Options popup gains "Restart into new UI". Only one UI
is created per boot, so RAM use is that of one.

Both UIs use the same stored machine connection: the new SettingsService reads and writes
machine slot 0 of the classic `fluidtouch` preferences. Settings that only the new UI has
live in their own namespace (`lrt`).

## 4. Services

| Service | Owns | Main operations |
|---------|------|-----------------|
| MachineService | Connection, status snapshot, events (state change, message, alarm, hold, probe result) | jog, cancel jog, home, unlock, zero, go to, hold, resume, reset, overrides, run file, send line |
| FileService | File sources and transfers | list(source, path), fetch to cache, upload to controller, delete; progress and cancel |
| JobService | The selected job: source file, cached copy, analysis (extents, stock, tools, operations, segment list), derived files, run state | select, analyse, transform, build outline, commit (upload), start, progress mapping |
| MacroService | Macro slots | list, run, edit |
| SettingsService | Persistent settings, export/import | get/set, backup to display SD |

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

PSRAM is 8 MB, but the build runs code from PSRAM (`CONFIG_SPIRAM_XIP_FROM_PSRAM`), which
takes roughly the firmware's size out of it, and LVGL has 1 MB. Free PSRAM on the device is
not yet measured, so the figures below are targets until spike S2:

| Use | Target |
|-----|--------|
| Preview canvas, RGB565, about 480 x 360 | 350 KB |
| Second canvas layer for cut progress | 350 KB |
| Segment list, 24 bytes per segment, arcs kept as arcs | 2 MB (about 87,000 segments; the reference job needs about 14,000) |
| Outline grids, 2 x 1 bit per cell, 4 M cells | 1 MB, only while tracing, when the canvas layers can be released |

If S2 shows less is free, the segment list moves to a file on the display SD read in
blocks, and the grid gets coarser.

## 7. Threads

| Context | Runs |
|---------|------|
| Main loop (core 1) | LVGL, FluidNCClient loop, service polling |
| Worker task (core 0), one at a time | fetch, analyse, transform, outline, upload |

Worker results are handed over by a flag the main loop checks; no LVGL calls from the worker.
