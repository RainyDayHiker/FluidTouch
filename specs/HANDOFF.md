# Handoff — state as of 2026-11-16

Written at the end of the planning sessions (done in a chat session with file access but no
git and no build tools) so a development session in VS Code can continue.

## 1. Where things stand

- **M0 (specs) is drafted.** Specs 01 to 09 are in this folder. Setup-screen and frame
  questions are answered. Job, Run and file-folder questions remain open
  ([07](07-decisions.md#open-questions)); none blocks M1 or M2.
- **No code has been written.** Nothing has been built, run in the simulator, or flashed
  as part of this work. All statements about the existing code come from reading it.
- **No spikes have been run** ([06 §2](06-test-and-spikes.md#2-spikes)).
- The specs files were written into the working tree **untracked**, while the branch
  `build/local-config-overrides` was checked out. They are not committed.

## 2. First steps for the development session

1. `git status`, `git remote -v`, `git log --oneline -5`, `git branch -vv`. Confirm the
   state described in §4 before changing anything.
2. Set up remotes and branches per [09-git-workflow.md](09-git-workflow.md) §1 and §2, and
   commit `CLAUDE.md` and `specs/` as the first commit on `lrt/main`.
3. Confirm the baseline builds before touching code: simulator (`.\sim\build.ps1 -Run`) and
   firmware (`pio run -e elecrow-crowpanel-7-advance-v13`). Record any problem here.
4. Start M1 ([08](08-milestones.md)), in this order:
   1. Listener interface in `FluidNCClient`, with the classic UI registered as a listener
      (behaviour unchanged). Verify classic UI in the simulator.
   2. UI mode key in NVS and the boot switch in `main.cpp`.
   3. `src/services` skeleton and `src/app` frame: tabs on top, status bar at the bottom,
      banners, five empty screens.
   4. Hidden test screen for spike S2 (free PSRAM, canvas draw timing).
   5. Fake FluidNC: job playback, `SD:` progress, `M0` hold, HTTP download/upload.
5. Ask Jeffrey to run the device-side spikes (S1 to S5) when the machine is on; write the
   results into [06](06-test-and-spikes.md).

## 3. People, places, files

| What | Where |
|------|-------|
| This repo | `D:\Source Code\Repos\FluidTouch` (origin: `github.com/RainyDayHiker/FluidTouch`) |
| Upstream | `github.com/jeyeager65/FluidTouch` (inferred from the README's web-installer link; confirm) |
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
- `src/network/fluidnc_client.cpp` calls into the UI directly in about six places
  (around lines 937, 975, 978, 1221, 1224: `UICommon` connection popups; around 1187:
  `UITabControlProbe::updateResult`). These are what the listener interface replaces.
  The header also includes `ui/machine_config.h`.
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
- `.github/workflows/` has `build-firmware.yml` and `build-branch-preview.yml`. Not read in
  detail: check what they trigger on and publish before pushing new branches.

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
- Whether `build/local-config-overrides` has commits that are not in upstream or in `main`.
- The upstream URL.

## 6. Planning artefacts outside the repo

An overview document ("LowRider Touch Controller: Recommendation, Specs and Build Plan")
exists in Jeffrey's Claude project "CNC Stuff". It predates several decisions; this folder
is the current source.

## 7. Keeping this file useful

Update §1 and §2 at the end of each working session (what was finished, what is next,
anything surprising). Move verified items out of §5 into the relevant spec.
