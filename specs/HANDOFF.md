# Handoff — state as of 2026-11-16 (end of the first development session)

Sections 1 and 2 are rewritten at the end of each working session.

## 1. Where things stand

**M1 (chassis) is done. The next session starts M2, the Setup screen.**

Git: only `main` (equal to `upstream/main`) and `lrt/main` exist, locally and on the fork,
in sync, working tree clean. CI (`build-lrt.yml`) runs on every push to `lrt/main`; see the
latest result with `gh run list --branch lrt/main --limit 1`.

What exists on `lrt/main`:

- **Platform edits** (all listed in [02 §2](02-architecture.md#2-changes-to-kept-code)):
  `FluidNCClient` reports to a listener; `main.cpp` starts the new UI or the classic UI by
  the UI mode key; the classic System Options dialog has "Restart into new UI"; the
  simulator builds from a path with a space and has `src/` on its include path.
- **`src/services`:** `SettingsService` (machine from classic slot 0) and `MachineService`
  (connection with its own retries, each attempt on its own task; status; stop, unlock,
  reconnect; last probe result). No File, Job or Macro service yet.
- **`src/app`:** the frame (tabs, status bar with STOP, alarm banner, connection banner that
  shows an attempt or counts down to the next one, no-machine banner), five placeholder
  screens, "Restart into classic UI" and the S2 draw test on the Settings placeholder.
- **Fake FluidNC** (`sim/tools/fake_fluidnc.py`): job playback, probe, `M0`, messages,
  overrides, reset, connection drop, and HTTP download, upload and file actions.
  `sim/tools/test_fake_fluidnc.py` checks it; `sim/tools/smoke_test.sh` starts both UIs
  without a window.
- **CI** for `lrt/**`: firmware, simulator with the smoke test, fake FluidNC test
  ([09 §7](09-git-workflow.md#7-ci)).

On the device (flashed by Jeffrey on 2026-11-16, at the commit before the last two changes):

- The new UI boots, connects to FluidNC and restarts into the classic UI. The way back is
  the System Options dialog (tap the right end of the classic status bar): a bit hidden,
  fine once known (D31).
- STOP touch area: good.
- Spike S2 run: 1.86 MB of PSRAM free; own drawing code 2.7 times faster than LVGL's.
  Results and conclusions in [06](06-test-and-spikes.md#s2-conclusions); memory budget in
  [02 §6](02-architecture.md#6-memory); D28, D29, D30.
- Spike S1: the `/sd/<path>` download and the HTTP folder listing work. Not yet tried while
  a job runs.
- With the controller off, each reconnect attempt froze the screen for 10 to 12 s.

**Built and tested in the simulator, but not yet on the device** (needs one more flash):

- The freeze fix: each connection attempt runs on its own task
  ([02 §4](02-architecture.md#4-services)).
- The banner showing "Connecting..." with Reconnect greyed out during an attempt, and
  "Next attempt in N s." between attempts.

To confirm after that flash: the screen stays responsive with the controller off, and
connecting still works with it on (the connect call now runs on the other core).

Known gaps, deliberately left:

- `MachineService::jobActive()` follows the `SD:` field, which FluidNC drops once it has
  read the last line of the file. At the end of a job the Run dot goes away while the last
  moves and the final `M0` are still to come. To be solved with the Run screen (M3); see
  [06](06-test-and-spikes.md#1-test-levels).
- Not tested anywhere: STOP as jog cancel (nothing in the new UI jogs yet; M2 will), loss
  of WiFi (the simulator's WiFi is always up), the classic UI's upload against the fake
  FluidNC's HTTP (it needs the fake on port 80).
- The fake FluidNC ignores the `P` word on `G38.2` and skips `$` lines such as `$HZ` inside
  a file; the reference job uses both.

Spikes ([06 §2](06-test-and-spikes.md#2-spikes)): S1 mostly done, S2 done. **S3, S4 and S5
are deferred** until their answers are needed (D32): S3 and S4 with M3, S5 before M8. Nothing
is waiting on Jeffrey for them now.

## 2. Next steps

1. **Start M2: the Setup screen** ([03 §2](03-screens.md#2-setup), decisions D11 and D18 to
   D25, [08](08-milestones.md)). On a branch `lrt/m2-<topic>` from `lrt/main`. Expected
   pieces:
   - `MachineService` actions for home, zero (all, XY, Z), jog and jog cancel, go to XY
     zero, laser offset move, probe Z, reset. The fake FluidNC already answers `$H`, `$J=`,
     `G10 L20`, `G38.2`, reset and jog cancel.
   - Settings the screen needs (jog steps and feeds, laser offset) in `SettingsService`:
     the jog values are in classic slot 0, the laser offset is new (`lrt` namespace,
     default X 33.6, Y -28.2).
   - The screen itself, replacing the Setup placeholder. Go through its control list with
     Jeffrey before building if anything in 03 §2 is unclear; M0's exit checklist still has
     "Screens reviewed one by one (Setup first)" open.
   - S3 in the requirements (no jog, zero or home while a job runs or is held).
2. **Flash once** at the start or end of that work, to confirm the two items in §1 that are
   not yet on the device. Steps: USB-C cable, then
   `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e elecrow-crowpanel-7-advance-v13 -t upload`.
   Tag `lrt-m1` once that flash checks out ([09 §6](09-git-workflow.md#6-tags-and-versions));
   no tag exists yet.
3. Small things still open from the device: S1 while a job runs, and the value of
   `$HTTP/BlockDuringMotion` on the controller.
4. Later, when their milestones come up: the S3 position log (with the Run screen, M3), S4
   (upload sizes, M3), S5 (OneDrive sketch and a Microsoft app registration, before M8).

How this session worked, for the next one: topic branches merged locally into `lrt/main`
with a merge commit and deleted; `platform:` and `sim:` commits kept separate from new
code; every change built for the simulator and the firmware and run in the simulator
before merging; specs updated in the same branch.

### Working notes for the simulator

- Firmware builds on this PC depend on `%USERPROFILE%\.platformio\penv` holding `pioarduino`
  6.2.0. If that folder is ever recreated (Python upgrade, deleted cache) the build fails
  with "not compatible with PlatformIO Core v6.2.0-post.1"; fix it with
  `%USERPROFILE%\.platformio\penv\Scripts\python -m pip install pioarduino==6.2.0`
  ([09 §7](09-git-workflow.md#7-ci)).
- MSYS2 is installed at `C:\msys64` (UCRT64 gcc, SDL2, cmake, ninja). `pio` is not on
  PATH; use `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`.
- To skip the Add Machine dialog in a fresh `--data` folder, put a
  `fluidtouch_settings.json` in its `sd/` folder (auto-import on first boot).
  `connection_type` 1 is WiFi (0 is UART). The import restarts the simulator, which drops
  the `--script` and leaves a second simulator process running; close it before
  rebuilding or the link step fails with "Permission denied".
- The classic UI stops reconnecting after a drop; the dialog's Connect button reconnects.
- The scenario scripts used this session (connect, alarm, drop, reconnect; a job with a
  forced drop; an address that does not answer) were temporary files and are not in the
  repo. `sim/tools/smoke_test.sh` shows the pattern: `--data <folder>`, `--script`, and
  `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software` for no window. An address such as
  `10.255.255.1` as the machine URL gives a connection attempt that hangs.
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
