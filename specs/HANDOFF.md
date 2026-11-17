# Handoff — state as of 2026-11-17 (end of the second development session)

Sections 1 and 2 are rewritten at the end of each working session.

## 1. Where things stand

**M1 (chassis) and the Setup screen of M2 are done and checked on the device. The next
session starts M3.**

Git: `main` (equal to `upstream/main`) and `lrt/main`, locally and on the fork, in sync.
CI (`build-lrt.yml`) passed on the last Setup commit. Tag `lrt-m2` marks the build on the
machine (the merge of `lrt/m2-setup`, 80b3abe). Use
`gh run list -R RainyDayHiker/FluidTouch --branch lrt/main --limit 1`: without `-R`, `gh`
looks at upstream's repository.

What exists on `lrt/main`, besides the M1 chassis (platform edits listed in
[02 §2](02-architecture.md#2-changes-to-kept-code), frame, banners, fake FluidNC, CI):

- **Setup screen** ([03 §2](03-screens.md#2-setup), D33 to D45): home all/Z/XY, zero
  all/Z/XY, Unlock, Reset; XY steps in a column and a square 3 x 3 jog pad that shows the XY
  feed, swapped for an XY joystick by a button under the steps; Z+/Z- with the Z feed between
  them and the Z steps; limit and probe pin dots; laser to bit and back; Probe Z pop-up
  (touch point as work and machine Z, then a 10 mm back-off).
- **Status bar** 64 px: state, WiFi, STOP full height; positions and the last FluidNC
  message between them; the display's own notices ("Wait for the jog to finish.") in amber.
- Buttons grey out only offline, in alarm and during a job; a press that can't go through
  now is not sent and the status bar says why (D38).
- **`MachineService`** actions and rules (`canHome`, `canZero`, `canMove`, `busyReason`);
  Home XY sends `$HX`, then `$HY` from `poll()`. **`SettingsService`** reads classic slot 0
  and the `lrt` keys (`laser_x`, `laser_y`, `probe_back`) once and keeps them in RAM.
- **Icons** of the new UI's own (zero, crosshair, end mill, joystick, arrows): A8 images
  drawn by `src/app/icons/make_icons.py`; run it after changing an icon.
- The fake FluidNC queues G-code lines sent back to back (needed for G91, G38.2, G0, G90).
- **Reconnecting backs off** (D46): retries after 5, 5, 10 and 10 s, then the banner asks
  for Reconnect. Checked in the simulator; not yet on the device (lrt-m2 predates it).

Checked on the device (2026-11-17): the reconnect fix (screen stays responsive with the
controller off), layout and touch sizes (could even go smaller), laser moves in both
directions, joystick, probe with back-off, Home XY, default steps from classic Settings >
Jog, no flicker on step presses.

Learned on the device: **any flash access (NVS read or write) makes the RGB panel flicker**,
because it stalls the panel's reads from PSRAM. Classic's `saveMachine()` rewrites every key
of every machine. The working screens therefore never write, and settings are read once (D44,
[02 §3](02-architecture.md#3-ui-mode)). The new Settings screen (M3) must save on an
explicit action and write only the keys that changed.

FluidNC has no command that homes X and Y together without Z: `$HXY` is not accepted. Home XY
stays X then Y (D37); `$H` homes XY in one cycle as the machine's config sets it.

Known gaps, deliberately left:

- `MachineService::jobActive()` follows the `SD:` field, which FluidNC drops once it has
  read the last line of the file; to be solved with the Run screen (M3).
- Settings are read once at boot: values changed in classic Settings show after a restart
  into the new UI, which is how the UI switch works anyway.
- Not tested: loss of WiFi (the simulator's WiFi is always up); STOP pressed during a jog on
  the device; the classic UI's upload against the fake FluidNC's HTTP.
- The fake FluidNC ignores the `P` word on `G38.2` and skips `$` lines inside a file.

Spikes: S1 mostly done, S2 done; S3 and S4 come with M3, S5 before M8 (D32).

## 2. Next steps

1. **Start M3** ([08](08-milestones.md)): Job list from the controller's and the display's
   SD (no preview), start panel, Run screen (Prompt state for `M0`, pause, abort, overrides,
   progress, times), Macros, Settings pages, upload from the display SD; S3 and S4. Review
   each screen's control list with Jeffrey before building it, as was done for Setup.
2. Settings (M3) takes over from classic Settings > Jog and Probe for: jog step lists and
   default steps, XY and Z jog feeds, joystick Max XY feed, probe feed and distance, laser
   offset, probe back-off. Save on an explicit action, changed keys only (D44, D45).
3. Small things still open from the device: S1 while a job runs, and the value of
   `$HTTP/BlockDuringMotion` on the controller.

How this session worked: topic branches merged locally into `lrt/main` with a merge commit
and deleted; `sim:` commits kept separate from new code; every change built for the
simulator and the firmware, run in the simulator (scripted clicks and screenshots), and the
screen reviewed by Jeffrey from screenshots before flashing.

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
- Click positions in scripts are screen coordinates: content y + 56 (the tab row).
  `press x y` repeated, then `release`, drags (used for the joystick).
- To see what a press redraws, a temporary `LV_EVENT_INVALIDATE_AREA` callback on the
  display that prints each area works well; remove it before committing.
- On the PC, `sim/build` builds from Git Bash with `export PATH="/c/msys64/ucrt64/bin:$PATH";
  cmake --build sim/build`; `sim/build.ps1` stops on a harmless CMake "GLOB mismatch" line.
- The device can talk to the fake FluidNC: run it on the PC (port 81, allow Python through
  the firewall) and make it the first machine in the classic list (Move Up); the new UI
  only uses the first one.
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
  flicker). Flash access still makes the panel flicker (D44).
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
