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

- Settings are read once at boot: values changed in classic Settings show after a restart
  into the new UI, which is how the UI switch works anyway.
- Not tested: loss of WiFi (the simulator's WiFi is always up; left until it shows up as a
  bug); the classic UI's upload against the fake FluidNC's HTTP.
- The fake FluidNC ignores the `P` word on `G38.2` and skips `$` lines inside a file.

Spikes: S1, S2, S3 and S4 done (S3 and S4 on 2026-11-18, results in [06](06-test-and-spikes.md), D64 to D66, Q22 open); S5 before M9, the inbox (D32, D78).

## 2. Next steps

1. **M3 in progress** ([08](08-milestones.md)). Job and Run (D47 to D53) and Settings
   (D54 to D58: Machine with a temporary slot picker, Jog & Probe, Display, Tools; saved
   on leaving Settings) are checked on the device by Jeffrey (2026-11-17, "all good") and
   merged into `lrt/main`. Layout tweaks to Job and Run wait for the preview. Macros
   (D59 to D61) is checked on the device by Jeffrey (2026-11-17, "all good") and merged
   (the fake FluidNC has sample macros in `/sd/fluidtouch/macros`, one holding at an M0).
   The position log (D62, Settings > Tools, CSV in `/logs` on the display SD) is built on
   merged. On the device it recorded the S3 run (2026-11-18): 1,322 rows, every message,
   no write error; Tools showed the status and no flicker was noticed (Jeffrey). S3 and S4
   are done and written up in [06](06-test-and-spikes.md); D64 to D66 follow from them, and
   Q22 (how Run finds the tool's line) is left open for M5. **M3's exit check is met**: a
   real job ran from the new UI with no fallback to classic (Jeffrey, 2026-11-18). Review
   each screen's control list with Jeffrey before building it.
2. **M4: the G-code library is built in CNC-Tools** (D67, [04](04-gcode-library.md)). Its
   spec is CNC-Tools `gcode-editor/specs/15-cpp-core.md`, revision 5 (2026-11-18, reviewed
   by both sides, no open questions; branch `spec/cpp-core` there). Phases: C0 (TS prep),
   C1 (read), C2 (Rotate, Flip only), C3 (outline); Move waits for the editor (D70). This
   repo's part starts when C1's release is promoted (D69): the copy script from a named
   release version, `.gitattributes` for the copies, the test firmware running the device
   suite from the SD card (results in 06), the SD reader and arena, the fallback font.
   **Library version in use: 0.0.1-build.64 (C1, promoted 2026-11-19).** Integrated:
   `tools/gcode_core.py` (fetch, check, data), `lib/gcode/` and `src/services/gcode_device/`
   copied and checked in CI, the library in the firmware and simulator builds, the device
   suite on Settings > Tools (D73). **The suite passes on the device** (57 of 57,
   2026-11-19, Jeffrey) as on the PC. Its `perf` figures are in 06: analysing the 789 KB
   reference job takes 8.4 s on the device, about half of it reading the card. A reader
   that scans 16 KB chunks (`lrt/m4-sd-reader`) gained only 4 to 8% (7.8 s): the display SD's
   SPI clock is 4 MHz (upstream's `UploadManager::init`), which caps reads near 500 KB/s.
   D74 raises it to 20 MHz, falling back to 4 MHz: on the device the suite passes 57 of 57
   at 20 MHz, reads at about 1 MB/s, and the reference job analyses in 5.0 s (06). Not yet
   checked on the device at 20 MHz: an upload from the display SD in the classic UI, other
   cards.
   **Library version in use: 0.1.0-build.70 (C2, Rotate and Flip, 2026-11-20)** (until 0.2.0, below). Copied and
   built (firmware and simulator); the suite passes 162 of 162 on the PC, in the simulator
   and **on the device** (6:03, 2026-11-20, Jeffrey). Rotate CW on the reference job: check
   8.0 s, apply 11.5 s (06). Budgets set: analysis D75, transforms D76 (04 §5). CNC-Tools answered the C2
   notes in spec 15 revision 11 (`applyChecked()`, `FileChanged`, the manifest's phase).
   **Library version in use: 0.2.0-build.84** (C2 with `applyChecked()`, 2026-11-21; a
   pre-release on GitHub): 162 of 162 on the PC, in the simulator and on the device (6:47,
   2026-11-21, Jeffrey). `applyChecked()` on the reference job: 3.7 s against apply()'s
   11.8 s, so a transform with its new analysis is about 17 s (06).
   **Library version in use: 0.3.0-build.87** (C3, the Outline Check, 2026-11-21, marked
   latest): 288 of 288 on the PC, in the simulator and on the device (8:31, 2026-11-21,
   Jeffrey). The outline in the preview's analysis (D77) makes it 68% longer for the
   reference job (7.7 s against 4.6 s); the arena peak, 321 KB, fits 512 KB (06). Kept so for
   now (D77, 2026-11-21); building it on request is the fallback if large previews feel slow.
   The library side of M4 to M7 is now in hand (C1 to C3).
   **Library version in use: 0.4.0-build.100** (New Surfacing Job, 2026-11-21, marked
   latest; also `fieldValue()` and the R half-circle fix, CNC-Tools D70, D71): 294 of 294 on
   the PC, in the simulator and on the device (9:01, 2026-11-21, Jeffrey). The largest
   surfacing job is written to the display SD in 0.28 s (06). The library side of M8 is in
   hand; its API matches spec 10 (`surfacingForm()`, `fieldValue()`, a 4 KB write buffer).
   The suite's SD writer now removes every `*.tmp` it closes (the new `perf-surfacing.tmp`
   as well as `perf-write.tmp`).
   **New jobs (M8, spec 10, D78)**, designed 2026-11-21: generators, starting with New
   Surfacing Job (CNC-Tools 15 §20, revision 15 reviewed; library 0.4.0 integrated, above). CNC-Tools
   answered `specs/outgoing/cnc-tools-generators.md` in spec 15 revision 16 (the form's texts
   in core, `fieldValue()`, §12.6's shape for every generator). Q26 and Q27 answered (D78): no copy
   kept on the display SD; New job is a card in the picker. **C3's design note (15 §18) is reviewed** (revision
   12, 2026-11-21, D77): the outline is built in the preview's analysis, identical to the
   editor's. Done ahead of C1: the fallback font and the
   display-text rule (D71), checked in the simulator only (a UTF-8 file name and message
   with `É – ° … “ ” — ½ © Å` on Job and Run; the Latin-1 conversion by a host test); not
   yet seen on the device.
3. **M5 (preview) designed** in [11](11-preview.md), 2026-11-21: Q22 answered as D79 (`Ln:`
   when the file has N words, else position matching), the time estimate on the display
   (D80), no "fits from here" (D81). Its §9 (P1 to P4) accepted (D82). **Part 5a** (fetch and
   analysis, `PreviewService`; branch `lrt/m5a-fetch-analyse`) is built and checked in the
   simulator (download, analysis, Cancel, Retry, a new choice mid-download, a name with
   spaces, a 404, no card, a choice during a cut). Checked on the device 2026-11-22
   (Jeffrey): the reference job analysed in 6.5 s with the outline (D75 expected about 7.7 s
   from 06's `perf`), Cancel and Retry, a display SD file analysed in place, and the library
   tests all passing with a job chosen. The download time wasn't noted. Merged.
   **Part 5b** (segment list, Job preview, info line and estimate; D83 to D88) is checked
   in the simulator (the reference jobs, an inch job, a 2 MB job truncated at 80%, zoom and
   Fit, the warnings, Cancel and Retry, the axis rates from the fake), by the host test of
   the packing (121 files), and on the device 2026-11-22 (Jeffrey): DogHoles downloads in
   1.6 s, analyses in 7.4 s (SD reads 1.2 s, packing 0.5 s), draws in 0.2 s; double tap
   zooms, drag moves when zoomed in (D85), Fit. Found and fixed on the device: the packer was
   slow from PSRAM (hot code in IRAM, tokens only on lines with F, N or P), the CPU caches
   at their largest (D87: the library back to 5a's 6.4 s), and LVGL's draw buffers 1.15 MB
   each instead of 768 KB (D88: the segment list now gets its 640 KB, 2.09 MB free before
   the arena). Download writes to the display SD failed three times with errno 5 (I/O) at
   31 KB, 175 KB and 718 KB, card nearly empty; the D86 retry recovered the last one on the
   device. Cause unknown; nearly every download so far was the first after a flash, so that
   says nothing. Merged 2026-11-22.
   **Part 5c** (Run tracking, [11 §6](11-preview.md#6-run-following-the-tool-d79), branch
   `lrt/m5c-run`), in four steps: (1) Run's preview with the cut so far, the marker, position
   matching, zoom and following (D89; built, checked in the simulator with the fake's
   playback and by the host test of `RunTracker`; not yet on the device), then D90 after
   Jeffrey's look at the screenshots (no file line, panel message or rapid override; STOP
   reads ABORT during a job; time left averaged and stopped while waiting); (2) the message
   following the tool, op N of M, time left from the estimate; (3) `Ln:`; (4) the download
   offer during a cut (D72).
   Open: the axis rates line ([11 §5](11-preview.md#5-the-info-line-and-the-time-estimate))
   on the real machine; Jeffrey sends it when he can.
4. The temporary machine picker (D57) is to be removed at the end of development.
5. Checked on the device 2026-11-18 (Jeffrey): a Controller SD file downloads during a cut
   (FluidNC serves `/sd/` through WebDAV, which ignores `$HTTP/BlockDuringMotion`; the
   setting is On), and STOP during a jog works. Loss of WiFi is not tested and left until it
   shows up as a bug.

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
  `FluidNCStatus.is_sd_printing / sd_percent / sd_filename`. No line number is reported
  (FluidNC sends `|Ln:` only with `use_line_numbers` on and N words in the file; the parser
  ignores it; Q22).
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
- OneDrive sign-in from the ESP32. Microsoft documents the device authorization grant as
  working with personal accounts; nothing was tried (S5).

## 6. Planning artefacts outside the repo

An overview document ("LowRider Touch Controller: Recommendation, Specs and Build Plan")
exists in Jeffrey's Claude project "CNC Stuff". It predates several decisions; this folder
is the current source.

## 7. Keeping this file useful

Update §1 and §2 at the end of each working session (what was finished, what is next,
anything surprising). Move verified items out of §5 into the relevant spec.
