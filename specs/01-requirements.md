# 01 — Requirements

## 1. Purpose

Replace the FluidTouch tab layout with screens that follow the order of a cutting job on one
LowRider v4 running FluidNC, and add file preview, simple transforms and an outline check on
the display itself.

## 2. Context and fixed points

| Item | Value |
|------|-------|
| Display | Elecrow CrowPanel 7" Advance v1.4 (ESP32-S3, 16 MB flash, 8 MB PSRAM, 800 x 480, GT911 touch), SD card fitted |
| Firmware target | `elecrow-crowpanel-7-advance-v13` only |
| Controller | FluidNC 4.x on the home WiFi; jobs run from FluidNC's SD card |
| Connection | WiFi (WebSocket + HTTP). Other connection types stay compiled, unused |
| Machines | One. No machine selection |
| G-code | FreeCAD CAM output from `lowrider_post.py`: `G10 L20 P0 X0 Y0 Z0` at start, `M0 (MSG ...)` prompts for bit insertion, probing and tool changes, `(MSG ...)` per operation, `(Stock: ...)` comment, G0 to G3, mm |
| Reference job | `Spoilboard_DogHoles.gcode`: 789 KB, 14,189 lines, 10,689 arcs. Largest so far; expect growth |

## 3. Goals

| ID | Requirement |
|----|-------------|
| G1 | **Setup on one screen:** home, unlock, jog XY and Z, zero, laser crosshair offset, probe Z, with no sub-tabs. |
| G2 | **Job on one screen:** pick a file, see a 2D preview with extents, rotate 90, flip, move (once the G-code Editor has it, D70), view and run an outline check, and select the job. |
| G3 | **Run on one screen:** state, position, current operation and messages, a preview showing cut progress, large Pause and Abort, feed and rapid overrides. |
| G4 | `M0` prompts are a normal step: the message and one Continue button, not a popup. |
| G5 | Macros (file-based, as now) and Settings remain available. |
| G6 | Transform results equal the CNC-Tools G-code Editor's output for the same file and operation. |
| G7 | The original job file is never modified; transforms write new files. |
| G8 | Capacity: any file size can be run. Transforms work on files up to at least 32 MB. Full-detail preview up to at least 5x the reference job; beyond that the preview is simplified and says so. |
| G9 | Getting a file from the PC or OneDrive to the machine does not require the PC to be in the workshop ([05](05-file-workflow.md)). |
| G10 | The classic UI stays reachable (restart into it from Settings) until explicitly retired. |
| G11 | Platform code (`src/core`, `src/network`, `sim/`) stays mergeable with upstream FluidTouch. |
| G12 | Everything runs in the desktop simulator against the fake FluidNC. |
| G13 | **New jobs from values:** make a complete job on the display from a few values, starting with the G-code Editor's surfacing job, equal to the editor's for the same values, then preview, transform, outline-check and run it like any other job. More such generators can be added from later library releases ([10](10-new-jobs.md)). |

## 4. Non-goals

- Streaming G-code line by line from the display. Jobs run from FluidNC's SD card.
- Editing G-code text, reordering operations, or 3D preview.
- Multi-machine support, A axis, X-only mode, joystick jogging in the new UI (still present in the classic UI).
- UART, USB CDC, ESP-NOW support in the new UI.
- CrowPanel Basic and Advance v1.2.

## 5. Safety requirements

| ID | Requirement |
|----|-------------|
| S1 | A stop control is on screen at all times: feed hold while running, jog cancel while jogging. |
| S2 | Actions take a single press, as in the classic UI (D18). Abort, Reset and Zero buttons are kept apart from frequently used buttons by layout. |
| S3 | While a job runs or is held, jog, zero, home, file and transform actions are disabled. |
| S4 | If the connection drops during a job, the screen says the job continues on the controller, and reconnects on its own for a while (retries after 5, 5, 10 and 10 s), then waits for Reconnect (D46). |
| S5 | The display never sends motion on its own initiative (no auto-home, no auto-move after connect). |
| S6 | A generated file (transform, outline, new job) is only offered to run after it was uploaded and its size confirmed on the controller. |

## 6. Acceptance for the whole project

1. From power-on, a real job is prepared, started, taken through its bit prompts and finished using only Setup, Job and Run.
2. Rotating the reference job on the display gives a file byte-identical to the G-code Editor's.
3. The outline check for a job runs as a job with Z raised and returns to the start.
4. The Run preview's cut portion follows the real tool within one operation's worth of path.
5. Classic UI can be entered and left without re-entering settings.
