# Handoff — state as of 2026-11-16 (end of first development session)

Sections 1 and 2 are rewritten at the end of each working session. The planning sessions
(chat, no git or build tools) wrote the first version; a development session in VS Code
has since done the setup and the first M1 step.

## 1. Where things stand

- **M0 (specs) is drafted.** Specs 01 to 09 are in this folder. Setup-screen and frame
  questions are answered. Job, Run and file-folder questions remain open
  ([07](07-decisions.md#open-questions)); none blocks M1 or M2.
- **Git is set up** per [09](09-git-workflow.md): `upstream` remote added (URL confirmed,
  `main` equals `upstream/main`), `lrt/main` created and pushed with the specs.
  The commit from `build/local-config-overrides` was not upstream, so it is part of
  `lrt/main`; that branch is deleted.
- **Baseline builds work.** Firmware `elecrow-crowpanel-7-advance-v13` builds (33.5% of
  flash). The simulator did not build from this checkout: the path contains a space and
  the forced include was passed unquoted. Fixed in commit
  `405a0ea` ("Sim: build from a checkout path that contains a space"), merged into
  `lrt/main`. It is not being offered upstream; its branch is deleted.
- **M1 steps 1 to 4 are done** and merged into `lrt/main`:
  1. `FluidNCClient` reports to a `FluidNCClientListener`; the classic UI registers
     `ClassicClientListener`.
  2. UI mode: NVS `lrt/ui_mode`, default `app`. `main.cpp` starts `LrtApp` or the classic
     UI. Classic System Options has "Restart into new UI"; the new Settings screen has
     "Restart into classic UI".
  3. `src/services` (`SettingsService`, `MachineService` with its own reconnecting
     connection logic) and the `src/app` frame: tabs, status bar with STOP, alarm banner
     with Unlock, connecting / connection-lost banner with Reconnect, no-machine banner,
     five placeholder screens.
  4. The S2 draw test screen (Settings > "Draw test (spike S2)"), with a synthetic job
     of the reference job's size. Runs in the simulator; its numbers only mean something
     on the device.
- **Tested in the simulator** against the fake FluidNC: classic connect, drop and reconnect
  unchanged (screenshots identical before and after the listener change); UI mode round
  trip; frame connecting, online, tab switching, STOP sends `!`, alarm banner and Unlock,
  drop and automatic reconnect, no-machine banner.
  With job playback in the fake: HOLD at an `M0` prompt, RUN and the dot on the Run tab,
  "The job continues on the controller" during `$sim/drop`, reconnecting mid-job, and the
  probe result reaching the client.
- **Known gap found with the fake:** `MachineService::jobActive()` follows the `SD:` field,
  which FluidNC drops when it has read the last line. At the end of a job the Run dot and
  the "job continues" line therefore go away while the last moves and the final `M0` are
  still to come. To be solved with the Run screen (M3); see
  [06](06-test-and-spikes.md#1-test-levels).
- **Not tested anywhere:** STOP as jog cancel (nothing in the new UI jogs yet); loss of
  WiFi (the simulator's WiFi is always up).
- **Not run on the device.** Firmware builds (33.7% of flash) but nothing has been flashed.
  First things to look at on the device: that it boots into the frame and connects; how
  long the screen freezes during a connection attempt while the controller is off
  ([02 §4](02-architecture.md#4-services)); the size of the STOP touch area.
- **No spikes have been run** ([06 §2](06-test-and-spikes.md#2-spikes)).
- CI: upstream's two workflows trigger on `v*.*.*` tags and on `feature/**`, `feat/**`,
  `fix/**`, `bugfix/**`, `dev` branches. Nothing triggers on `lrt/**` or `lrt-*` tags, so
  pushing those publishes nothing. `fix/<topic>` branches do match; see
  [09 §2](09-git-workflow.md#2-branches). No `lrt/**` workflow exists yet
  ([09 §7](09-git-workflow.md#7-ci)).

## 2. Next steps

1. Continue M1 ([08](08-milestones.md)), in this order, each on its own
   `lrt/m1-<topic>` branch:
   1. Fake FluidNC: HTTP download and upload (job playback is done).
   2. `build:` commit adding a CI workflow for `lrt/**`.
2. Flash the M1 build and go through the device points in §1. Run spike S2 from
   Settings > "Draw test (spike S2)" and fill in the table in
   [06](06-test-and-spikes.md#s2-procedure).
3. Ask Jeffrey to run the device-side spikes (S1 to S5) when the machine is on; write the
   results into [06](06-test-and-spikes.md).

### Working notes for the simulator

- MSYS2 is installed at `C:\msys64` (UCRT64 gcc, SDL2, cmake, ninja). `pio` is not on
  PATH; use `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`.
- To skip the Add Machine dialog in a fresh `--data` folder, put a
  `fluidtouch_settings.json` in its `sd/` folder (auto-import on first boot).
  `connection_type` 1 is WiFi (0 is UART). The import restarts the simulator, which drops
  the `--script` and leaves a second simulator process running; close it before
  rebuilding or the link step fails with "Permission denied".
- The classic UI stops reconnecting after a drop; the dialog's Connect button reconnects.
- The simulator starts in the new UI. Type `$sim/alarm=1` and the like into the fake
  FluidNC's own console; the new UI has no terminal yet.
- Scripted runs: start the fake with a timed stdin, e.g.
  `( sleep 10; echo '$sim/alarm=1'; sleep 30 ) | python sim/tools/fake_fluidnc.py --port 81`,
  and the simulator with `--script`. The Windows Store Python runs as `python3.10.exe`;
  look for that name when a stale fake still holds the port.

## 3. People, places, files

| What | Where |
|------|-------|
| This repo | `D:\Source Code\Repos\FluidTouch` (origin: `github.com/RainyDayHiker/FluidTouch`) |
| Upstream | `github.com/jeyeager65/FluidTouch` (remote `upstream`) |
| CNC-Tools (G-code Editor, TypeScript core and its specs) | `D:\Source Code\Repos\CNC-Tools\gcode-editor` |
| Reference job (789 KB, 14,189 lines, ~75% arcs) | `%OneDrive%\Hobbies\CNC\Spoilboard\Spoilboard_DogHoles.gcode` |
| Laser macro in use today | `%OneDrive%\Hobbies\CNC\Config\FluidTouch\macros\laser_offset.gcode` (`G91`, `G0 X33.6 Y-28.2`, `G90`) |
| Post-processor that writes the jobs | `%APPDATA%\FreeCAD\v1-1\Macro\lowrider_post.py` |
| Hardware | CrowPanel 7" Advance v1.4 with SD card; LowRider v4; FluidNC 4.x on home WiFi; an always-on PC syncing OneDrive |

How Jeffrey works a session: power on, home all, zero all, add stock, jog to the start
point (using the laser crosshair), start the job. Jobs zero themselves again at the start
and pause with `M0 (MSG ...)` for bit insertion, probing and tool changes.

## 4. Findings from reading the code (verify line numbers; they drift)

**FluidTouch**

- Version 1.1.0; LVGL 9.5.0, LovyanGFX 1.2.29, ArduinoWebsockets 0.5.4, ArduinoJson 7.4.3;
  pioarduino platform 55.03.312-1.
- `custom_sdkconfig = CONFIG_SPIRAM_XIP_FROM_PSRAM=y` (code runs from PSRAM; fixes display
  flicker). This uses PSRAM; free PSRAM is unmeasured (spike S2).
- `include/lv_conf.h`: `LV_MEM_SIZE` 1 MB, `LV_USE_CANVAS 1`, 16-bit colour, no vector graphics.
- `src/network/fluidnc_client.cpp` called into the UI directly in six places (connection
  popups, probe result). Replaced by `FluidNCClientListener` in M1. The header still
  includes `ui/machine_config.h`.
- Job progress: status field `SD:<percent>,<filename>` parsed around line 1121 into
  `FluidNCStatus.is_sd_printing / sd_percent / sd_filename`. No line number is reported.
- `FluidNCStatus::isIdle()` treats a running file as busy even when FluidNC reports Idle
  during a `G4` dwell. Keep that.
- File list: `$Files/ListGcode=<path>` (JSON reply). Run: `$SD/Run=<path>` /
  `$LocalFS/Run=<path>`. Macros: `$SD/Run=/sd/fluidtouch/macros/<file>`.
- Upload: `src/ui/upload_manager.cpp`, HTTP multipart POST to `/upload` (SD) or `/files`
  (flash), in a FreeRTOS task on core 0 with a polled state struct. Create-dir via
  `?action=createdir`. There is no download code.
- UI classes are static, one per tab; big files: `ui_tab_files.cpp` 101 KB,
  `ui_common.cpp` 83 KB, `ui_tab_status.cpp` 79 KB, `ui_machine_select.cpp` 63 KB.
- Simulator compiles the real `src/ui`, `main.cpp` and `fluidnc_client.cpp` against shims in
  `sim/`; `sim/tools/fake_fluidnc.py` handles status, `$G`, jog, home, zero, overrides,
  hold/resume, file listing. It does **not** run jobs or serve HTTP.
- `.github/workflows/` has `build-firmware.yml` (tags `v*.*.*`: release and web installer)
  and `build-branch-preview.yml` (feature/fix/dev branches: preview on gh-pages). See §1.

**CNC-Tools `gcode-editor/packages/core/src`** (what `lib/gcode` ports)

`parse/tokenizer.ts`, `parse/vocabulary.ts`, `num/fixed.ts`, `sim/simulator.ts`,
`sim/arc.ts`, `ops/classify.ts`, `ops/rotate90.ts` (rotate and flip share it),
`ops/flip.ts`, `ops/stock.ts`, `ops/fixed-moves.ts`, `ops/structure.ts`,
`ops/outline-check.ts`. Tests and fixtures are in `packages/core/test`. A translate/move
operation does not exist there yet.

## 5. Not verified

- That FluidNC serves SD files at `http://<address>/sd/<path>` (spike S1). Only indirect
  evidence was found.
- Free PSRAM and canvas drawing speed on the device (S2).
- How closely `SD:` percent tracks the line being cut (S3).
- Upload reliability for multi-MB files (S4).
- OneDrive sign-in from the ESP32. Microsoft documents the device authorization grant as
  working with personal accounts; nothing was tried (S5).

## 6. Planning artefacts outside the repo

An overview document ("LowRider Touch Controller: Recommendation, Specs and Build Plan")
exists in Jeffrey's Claude project "CNC Stuff". It predates several decisions; this folder
is the current source.

## 7. Keeping this file useful

Update §1 and §2 at the end of each working session (what was finished, what is next,
anything surprising). Move verified items out of §5 into the relevant spec.
