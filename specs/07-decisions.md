# 07 — Decisions and open questions

## Decision log

| # | Decision | Instead of | Why |
|---|----------|-----------|-----|
| D1 | Build on the existing fork: keep `src/core`, `src/network`, `sim/`; write a new UI in `src/app` (2026-11-16) | Modifying the classic tabs; a new firmware from zero | The classic UI is ~600 KB organised around the layout being replaced; the platform layers are layout-independent and proven |
| D2 | Stay mergeable with upstream for platform code; edits to kept files are minimal and listed in [02 §2](02-architecture.md#2-changes-to-kept-code) | Diverging freely | Upstream fixes to drivers and client are wanted |
| D3 | The classic UI stays in the firmware, selected at boot | Deleting it once the new screens exist | A fallback for controls not yet carried over; also keeps `src/ui` unmodified, which helps D2 |
| D4 | WiFi only in the new UI | ESP-NOW or wired | Preview, transforms and uploads need file transfer; ESP-NOW has none |
| D5 | One machine; connection settings shared with classic slot 0 (a temporary slot picker for development, D57) | Separate settings | No re-entry when switching UI |
| D6 | G-code logic is a C++ port of the CNC-Tools core, proven equal by golden cases exported from the TypeScript | Running the TypeScript on device; making C++ the single source for both | A JS engine does not fit the memory and speed budget; rewriting a working desktop app is out of scope |
| D7 | The library streams; no whole-file model | Porting the document model | 8 MB PSRAM shared with display and code |
| D8 | The display SD holds the working copy; FluidNC's SD holds what runs | Working in PSRAM; transforming on the controller's card | File size is then not limited by RAM; originals stay untouched |
| D9 | Transforms write a new file with a suffix; the original is never overwritten | In-place | Safety and traceability |
| D10 | Outline check is generated on the device and run as a job from the controller's SD, following CNC-Tools spec 13's file format | Streaming jog moves; bounding box only | Asked for as a job; one behaviour across PC and display |
| D11 | Laser offset is a setting (default X 33.6, Y -28.2 from `laser_offset.gcode`) sent as a relative jog | Running the macro file | Cancellable, leaves modal state alone, no trip to Macros |
| D12 | `M0` with a message is a Prompt state on the Run screen | The classic HOLD popup | The jobs pause by design several times each |
| D13 | Macros keep the classic folder `/sd/fluidtouch/macros/` | A new folder | Shared by both UIs |
| D14 | Preview segments keep arcs as arcs | Flattening at parse time | The reference job is 75% arcs; flattened it would be several times larger |
| D15 | Move is specified in CNC-Tools first and implemented in both | Device-only operation | D6 |
| D16 | Files reach the display first (inbox, card, or controller), and are uploaded to the controller as the last step | PC pushes to FluidNC | The machine and the PC are rarely on together |
| D17 | Inbox: OneDrive directly, with the PC folder server as fallback (an always-on PC that syncs OneDrive exists) (2026-11-16) | PC folder server first | No PC dependency; internet is available in the workshop. Subject to spike S5 |
| D18 | Actions take a single press; no press-and-hold or confirm | Hold-to-activate for abort, zero, home, start | Matches the classic UI, which has caused no problems |
| D19 | Setup follows the session order: Home all, Zero all, then jog and laser, then Start. Zero XY and Zero Z stay, smaller | Hiding manual zero | Zero all is part of every session; the others are occasional |
| D20 | Jog steps: XY 100, 50, 10, 1, 0.1 mm; Z 50, 25, 10, 1, 0.1 mm, each with its own selector | One shared list | Z never needs 100 |
| D21 | Laser moves in both directions | One direction | Fits; occasionally useful |
| D22 | Abort is a soft reset | Pause, then stop and raise | Abort is used when something has gone wrong |
| D23 | Setup has Reset and a Probe Z panel; jog to typed coordinates is not carried over | | Soft reset and manual Z probe are used; typed jog is not |
| D24 | Probe Z reports the value only, for now. **Changed by D43**: it backs off after the touch | Set zero and retract | Asked for; can be extended later |
| D25 | Go to XY zero has no Z height check, for now. **Moot since D40**: the button was removed | Warn or refuse when Z is low | Not needed yet |
| D26 | The status bar shows work and machine position at all times | Work only, tap for machine | Asked for |
| D27 | Screen tabs are at the top; the status bar (state, positions, WiFi, STOP) is at the bottom ([03 §1](03-screens.md#1-frame)) | Status in a top bar with navigation elsewhere | Matches the classic layout, so the positions stay where they are read today |
| D28 | The preview is drawn by its own code writing pixels into the canvas buffer (2026-11-16) | LVGL's line and arc draw functions | Spike S2 on the device: 210 ms against 562 ms for a reference-sized job |
| D29 | Cut progress on Run is drawn incrementally onto its layer; a full redraw only when the view changes (2026-11-16) | Redrawing the progress layer on every update | Spike S2: a full redraw costs about 320 ms |
| D30 | Preview segments are 8 bytes each, held in RAM: end point as two 16-bit values scaled to the job's extents, start taken from the previous segment, 16-bit radius, 16 bits of flags and depth level. A file on the display SD is the fallback for jobs that still don't fit (2026-11-16, was Q21) | 24-byte segments; SD file as the normal case; freeing memory in the platform | 1.86 MB of PSRAM is free (S2); 8 bytes holds about ten times the reference job without touching upstream's display buffers |
| D31 | "Restart into new UI" in the classic UI stays in the System Options dialog only (tap the right end of the classic status bar) (2026-11-16) | Also adding it to the classic Settings tab | Found to be a bit hidden, but fine once known; no further edit to upstream files |
| D32 | Spikes S3, S4 and S5 are run when their answers are needed: S3 and S4 with M3, S5 before M8 (2026-11-16) | All spikes at the start of M1, as first planned | S3 needs a position log and a started job, S4 an upload from the new UI, S5 a sketch and an app registration; none of them affects M2 |
| D33 | Setup has no job strip: no selected job, "fits from here", Choose job or Start. The Job tab picks the job and shows whether it fits; Run monitors it (2026-11-17) | Keeping the strip on Setup, at first with its space reserved until M3 | Seen on screen, it duplicated the Job tab and crowded the jog controls |
| D34 | Probe Z shows the touch point as work Z and machine Z (2026-11-17) | One of the two | Work Z is what is read during set-up; machine Z is what FluidNC reports and stays comparable across re-zeroing |
| D35 | The XY and Z jog feeds are not on Setup; they are set in Settings (2026-11-17) | [-]/[+] feed controls on Setup | Not changed on the fly; frees room for the jog pad |
| D36 | The jog step chosen on Setup is saved to classic slot 0 (2026-11-17). **Replaced by D45** | The new UI's own `lrt` settings; session only | Both UIs remember the same values, as with the connection (D5) |
| D37 | Setup has Home XY: `$HX`, then `$HY` once the X cycle has ended (2026-11-17) | `$H` only; Home X and Home Y as separate buttons | Re-homing XY without touching Z. The classic UI uses the same commands; FluidNC takes one named axis per `$H` |
| D38 | Setup greys out buttons only in lasting states (offline, alarm, job). A press that can't be sent at the moment (e.g. Zero during a jog) is not sent, and the status bar says why (2026-11-17) | Greying out by the current state, including JOG and HOME; queueing the press until idle | Greying out by state made the screen flicker during every jog |
| D39 | The status bar is 64 px: state, WiFi and STOP full height; between them, positions on one row and the last FluidNC message on the other. Content is 800 x 360 (2026-11-17) | 48 px bar without messages | The classic UI's message box is useful |
| D40 | Setup has no Go to XY zero and no feed controls; Z+/Z- are left of the Z steps; home and zero buttons show icons (2026-11-17) | The first M2 layout | Seen on screen: rarely used controls removed, more room for jogging |
| D41 | Setup, second pass: the XY steps are a column left of a square jog pad; the pad's centre shows the XY jog feed and the gap between Z+ and Z- the Z jog feed; Z+/Z- and the step buttons are narrower; laser and bit moves are two equal buttons drawn as crosshair and end mill icons; zero is a slashed-zero icon drawn for this UI (`src/app/icons/make_icons.py`) (2026-11-17) | Steps in a row above the pad; the step shown in the pad's centre; a small "b>l" text button (D40) | Seen on screen: the step is already visible on its selector, the feed is not; square arrows; space around the controls |
| D42 | Setup has an XY joystick, swapped with the jog pad by a button under the XY steps. It works as the classic joystick tab's XY mode (short jogs every 50 ms, quadratic response, classic's joystick Max XY feed, jog cancel on release), without X-only/Y-only, Z or speed display. The mode is not remembered across restarts (2026-11-17) | Leaving the joystick in the classic UI only | Asked for: continuous positioning without choosing steps |
| D43 | After a touch, Probe Z raises Z by a back-off of the new UI's own (`lrt` `probe_back`, default 10 mm), sent as `G0 Z..` queued behind `G38.2` as classic's retract is (2026-11-17) | No retract (D24); classic's retract setting (2 mm by default) | Tried on the device: the bit should come clear of the plate |
| D44 | The new UI keeps slot 0 and its `lrt` settings in RAM after the first read; the working screens don't write to flash (2026-11-17) | Reading and writing settings on each press | On the device every step press made the screen flicker: saving went through classic's `saveMachine()`, which rewrites every key of every machine, and flash access stalls the panel's PSRAM reads. Writing one key 5 s later still flickered visibly |
| D45 | The default jog steps are set in Settings (classic Settings > Jog until M3); Setup starts with them, and a step chosen on Setup lasts until a restart (2026-11-17) | Saving the step chosen on Setup (D36) | Settings is the one place that saves; no flash writes, so no flicker, during setup |
| D46 | Reconnecting backs off: retries after 5, 5, 10 and 10 s, then the banner asks for Reconnect, which starts the same sequence again. A successful connection or WiFi coming back resets it. WiFi joining is unchanged (every 15 s, without limit) (2026-11-17) | Retrying every 5 s without end | Asked for: a controller that is off should not be retried for ever |
| D47 | M3 builds Job (no preview) and Run first, then Settings, then Macros; each screen's controls are reviewed before it is built (2026-11-17) | Settings first; Run alone first | A real job from file to finish is the core of M3 |
| D48 | Job's top line is the chosen file; tapping it opens a file picker over the screen. The rest of Job is for preview, transforms and "Use this job" (2026-11-17) | A file list beside the preview, as first drawn | The list is used once per job; the preview and transforms need the room |
| D49 | The picker browses folders on both cards; folders first, then files, by name A to Z (2026-11-17) | Newest first | `$Files/ListGcode` gives no dates, and FluidNC has no clock |
| D50 | The picker deletes files, after a confirm (2026-11-17) | Single press (D18) | Clearing old files is a common step; a deleted file can't be brought back. D18 was about machine actions |
| D51 | "Use this job" opens Run's start panel: file and a large START, nothing else until M5. Until the preview exists, Run's preview area shows the file and the current message in large type; time left is estimated from the `SD:` percent, marked ~ (2026-11-17) | Starting from Job; a full-width Run layout until M5; elapsed time only | Starting stays on Run; the layout doesn't change when the preview arrives |
| D52 | Run: the final `M0` is a Prompt like the others, and CONTINUE then Finished ends the job (was Q18). Feed override in 10% steps only (was Q19) (2026-11-17) | Finished at the last `M0`; 1% steps as well | |
| D53 | Uploaded and generated jobs go to `/sd/fluidtouch/uploads/` on the controller (was Q7) (2026-11-17) | `/sd/lrt/`; the SD root | The classic UI's upload already uses it: one place for both UIs |
| D54 | Settings pages for M3: Machine, Jog & Probe (the Laser page folded in), Display, Tools (2026-11-17) | The six pages of the first spec | The Job picker already covers listing and deleting files; travel, terminal, backup and the inbox wait for the features that use them |
| D55 | Settings saves when it is left (another tab, or a restart from Tools), writing only the keys that changed; no Save button (2026-11-17) | Save/Revert per page; one Save button | Fewest presses. Flash writes flicker the panel (D44), so nothing is written per tap |
| D56 | Numbers are entered on a keypad pop-up, text on a keyboard pop-up (2026-11-17) | +/- nudges beside the values | Settings are set once and rarely; exact values matter more than speed |
| D57 | A temporary machine picker on Settings > Machine: the new UI uses the classic slot chosen there (`lrt` key `machine_slot`, default 0) instead of always slot 0 (2026-11-17) | Slot 0 only (D5) | During development the display switches between the real machine and the fake FluidNC. To be removed at the end, going back to D5 |
| D58 | Settings review: Jog & Probe only chooses the start steps (the step lists are not edited); jog feeds get -/+ 100 buttons beside the keypad value; brightness is a slider (25 to 100%), and the dimmed level is no longer a setting: a quarter of the brightness, at least 5% (2026-11-17) | Editable step lists; keypad only (D56); four brightness choices and a dimmed-brightness choice | The step lists don't need changing; feeds are nudged more than typed; fewer settings |
| D59 | The new UI keeps its own list of 12 macros in the `lrt` namespace, one list (not per machine slot), starting empty (2026-11-17) | Share classic's 9-slot list (`m<slot>_macros`); copy classic's once | 12 slots and command macros don't fit classic's record; Jeffrey sets the few up again |
| D60 | A macro is a file in the macros folder or one command line | Files only | Short things such as `$H` or a G0 to a park position need no file |
| D61 | A running macro stays on Macros (strip with progress, last message, Resume at a hold, Stop as a soft reset); Run does not take macro files as jobs. Edit mode: tap a slot for a pop-up (name, file or command, colour, move left/right, clear), saved on Done or leaving | Treat a macro as a job on Run; classic's list editor with up/down/edit/delete per row | A macro is short and run from Macros; Run stays the job's |

## Open questions

Answered ones move into the log above.

| # | Question | Draft assumes |
|---|----------|---------------|
| Q2 | Is a one-time phone sign-in for OneDrive acceptable, repeated only if the display is unused for months? | Yes |
| Q3 | Which folder should be the inbox (e.g. `OneDrive\Hobbies\CNC\To Cut`)? Flat, or with subfolders? | One flat folder |
| Q4 | Want "Save to CNC inbox" in the G-code Editor? | Yes, late |
| Q6 | Offer clean-up of old uploaded jobs on the controller's SD? | No |
| Q14 | Rough time estimate on Job and Run? | Yes |
| Q15 | Preview colours: by depth, or by operation/tool? | Depth |
| Q16 | Move: typed values only, or nudge buttons too? | Typed |
| Q20 | Move producing negative coordinates: block or warn? (Your jobs keep all moves positive.) | Warn |
