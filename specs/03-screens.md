# 03 — Screens

First draft. Each screen has a wireframe, a control table, and questions. Sizes are in
pixels on the 800 x 480 display. Touch targets are at least 64 x 56.

## 1. Frame

```
+--------------------------------------------------------------------------------+
|   Setup    |     Job     |     Run     |    Macros    |   Settings             | 56
+--------------------------------------------------------------------------------+
|                                                                                |
|                              screen content  800 x 360                         |
|                                                                                |
+--------------------------------------------------------------------------------+
|       | W X123.45 Y67.89 Z15.00   M X223.45 Y167.89 Z-5.00  |      |           | 64
| IDLE  | Probe done. Next: insert the 6 mm end mill            | wifi | [ STOP ]  |
+--------------------------------------------------------------------------------+
```

Screen tabs are at the top; the status bar is at the bottom (D27). State, WiFi and STOP
take the bar's full height; between them, positions are on the first row and the last
message on the second (D39).

| Element | Behaviour |
|---------|-----------|
| State | IDLE, RUN, HOLD, JOG, HOME, ALARM, OFFLINE; colour coded |
| Position | Work position and machine position, both always visible, in the status bar |
| Message | The last `[MSG:...]` from FluidNC, as in the classic UI's message box; it stays until the next one. A notice of the display's own (e.g. "Wait for the jog to finish.") is shown there in amber for 5 s, or until FluidNC sends a message (D38, D39) |
| STOP | Jogging: jog cancel. Otherwise feed hold (`!`), which FluidNC ignores when idle. Always enabled. Resume is only on the Run screen, so it can't be hit by habit. The button is 104 x 52; its touch area extends 10 px beyond it on every side. The 104 x 40 button in the 48 px bar was checked on the device: good |
| Alarm banner | Replaces the status bar: alarm text + Unlock (`$X`). On every screen |
| Offline banner | Replaces the status bar. The text says whether an attempt is under way or when the next one starts. During an attempt: "Connecting to <machine>..." before the first connection, "Connection lost. Reconnecting..." after a drop; Reconnect is greyed out, because it could do nothing. Between attempts: "No answer from <machine>. Next attempt in N s." or "Connection lost. Next attempt in N s.", counting down; Reconnect starts the attempt at once. Once the retries are used up (after 5, 5, 10 and 10 s, D46): "No answer from <machine>. Press Reconnect to try again." or "Connection lost. Press Reconnect to try again."; Reconnect starts again with the same back-off. While joining WiFi: "Connecting to WiFi <ssid>..." (or "WiFi lost. Reconnecting to <ssid>..."). If a job was running when the link dropped, a second line "The job continues on the controller." Retries happen on their own until then ([02 §4](02-architecture.md#4-services)) |
| No-machine banner | The chosen classic machine slot (D57) is empty or not a WiFi machine: says so, with a Settings button that opens Settings > Machine |
| Navigation | Always available. A dot on Run while a job is active. Starting a job switches to Run |

## 2. Setup

Laid out left to right in the order of a session: power on, **home all**, **zero all**, add
stock, **jog** to the start point (with the laser). Choosing and starting the job are on
the Job tab (D33).

```
+--------------------------------------------------------------------------------+
| [H All] [0 All] | [H Z] [0 Z] [H XY] [0 XY] |                 [Unlock] [Reset]  |
+--------------------------------------------+----------------+-----------------+
| [100] |  [<^]   [ ^ ]   [^>]               |  [ Z+ ] [50]   | Limits o X o Y o Z |
| [50]  |                                    |  [    ] [25]   | o Probe          |
| [10]  |  [ < ]  3000    [ > ]              |   1000  [10]   | [ (+) > bit ]    |
| [1]   |         mm/min                     |  mm/min [1]    | [ bit > (+) ]    |
| [0.1] |  [<v]   [ v ]   [v>]               |  [ Z- ] [0.1]  | [ Probe Z ]      |
| [J]   |                                    |                |                  |
+--------------------------------------------+----------------+-----------------+
  H = home icon, 0 = zero icon (a slashed zero), (+) = laser crosshair icon,
  bit = end mill icon, J = joystick icon; arrows as in the classic jog tab

In joystick mode the pad (arrows and feed) is replaced by a round XY joystick, the XY steps
are greyed out, and [J] shows a four-arrows icon that switches back (D42).
```

Exact positions are settled when the screen is built; the order and grouping above are the
spec. Home buttons are green and zero buttons amber; the jog pad is coloured by axis as in
the classic jog tab (X blue, Y green, diagonals teal, Z purple). The jog pad's buttons are
square; Z+/Z- and the step buttons are no larger than needed, leaving space around the
groups (D41).

| Control | Sends | Notes |
|---------|-------|-------|
| Home all / Home Z | `$H` / `$HZ` | Single press |
| Home XY | `$HX`, then `$HY` | FluidNC homes one named axis per `$H` command and refuses another while one runs, so `$HY` is sent once the X cycle has been seen and has ended (back to Idle). An alarm, a reset or a lost connection cancels the `$HY`; so does `$HX` not showing as Home within 3 s (D37) |
| Zero all / Zero Z / Zero XY | `G10 L20 P0 X0 Y0 Z0` / `... Z0` / `... X0 Y0` | Zero all is the everyday one; the other two are smaller |
| Unlock | `$X` | Highlighted in ALARM |
| Reset | soft reset (0x18) | Kept at the far right, away from the jog pad |
| Jog pad | `$J=G91 G21 X.. Y.. F..` | One step from the XY selector per press. The diagonals move one step in X and one in Y. The centre shows the XY jog feed (D41). Presses queue on FluidNC (five presses of 100 mm move 500 mm); STOP cancels them all |
| Arrows / joystick switch | | Under the XY steps; an icon of the mode it switches to. Not remembered: Setup starts with the arrows after a restart (D42) |
| XY joystick | `$J=G91 G21 X.. Y.. F..` every 50 ms while held, jog cancel (0x85) when let go | As the classic joystick tab, XY only: no X-only/Y-only modes, no Z joystick, no speed display. Touching anywhere on the circle moves the knob there. Speed follows the knob's distance from the centre, squared (fine near the centre), up to the classic joystick "Max" XY feed (classic Settings > Jog). Each jog covers the time since the last, so the machine follows the knob. A refused press (e.g. homing) shows the reason, as for other buttons (D38) |
| Z+/Z- | `$J=G91 G21 Z.. F..` | Left of the Z step selector; the Z jog feed is shown between them (D41) |
| Step selectors | | XY: 100, 50, 10, 1, 0.1 mm, in a column left of the pad. Z: 50, 25, 10, 1, 0.1 mm, in a column right of Z+/Z-. The lists are editable in Settings. Setup starts with the default XY and Z steps from Settings (classic Settings > Jog until M3). A step chosen on Setup lasts until a restart and is not saved: Setup never writes to flash (D45) |
| Jog feeds | | Shown on Setup, not changed there. XY and Z jog feeds are set in Settings (classic Settings > Jog until the new Settings page exists, M3) (D35) |
| Laser -> bit | `$J=G91 G21 X33.6 Y-28.2 F..` | Shown as crosshair icon > end mill icon. Offset from Settings (defaults from `laser_offset.gcode`). A jog, so it can be cancelled and leaves G90/G91 alone. XY jog feed |
| Bit -> laser | The reverse move | End mill icon > crosshair icon, same size as Laser -> bit (D21, D41) |
| Probe Z | Opens a pop-up: what it will do (feed, max distance from Settings), [Probe], [Close], result line | Sends `G91`, `G38.2 Z-<max> F<feed>`, `G0 Z<back-off>` (10 mm, its own setting, D43), then restores the G90/G91 that was active. The back-off is queued behind the probe, as classic's retract is, so a miss (alarm) discards it. Classic's probe thickness and retract are not used. Reports the touch point: it does not set zero. Result: "Touched at Z -1.234 work (-81.234 machine)", or "No contact within 10 mm" (D34). For levelling, squaring and test work; jobs probe themselves |
| Limit and probe dots | | From the `Pn:` field of the status report; lit while the pin is active |

Buttons are greyed out only in states that last, so nothing flickers while jogging (D38):

| State | Greyed out |
|-------|------------|
| Offline (any banner but alarm) | Everything |
| ALARM | Everything but Home all, Home Z, Home XY, Unlock, Reset |
| A job runs or is held (S3) | Everything but Unlock, Reset |
| Otherwise (IDLE, JOG, HOME, HOLD without a job, a probe under way) | Nothing |

In the last row, a press that cannot be sent at that moment is not sent; the status bar
shows why instead: "Wait for the jog to finish.", "Wait for homing to finish.", "Wait for
the probe to finish.", "Wait for the machine to stop.", or "The machine is in hold. Reset
or STOP first." Jog and laser presses during a jog are sent and queue on FluidNC. The
services check the same rules before sending anything ([02 §4](02-architecture.md#4-services)).

Not carried over (still in the classic UI): go to XY zero (D25), jog to typed coordinates,
joystick, X/Y probing.

## 3. Job

The top line is the chosen file; tapping it opens the file picker. The rest of the screen
previews that file, transforms it, and hands it to Run (D48).

```
+--------------------------------------------------------------------------------+
| Spoilboard_DogHoles.gcode    789 KB    Controller SD  /jobs           [Change] |
|--------------------------------------------------------------------------------|
|                                                                                |
|                          preview  ~784 x 220   (M5)                            |
|                                                                                |
|--------------------------------------------------------------------------------|
| 520 x 864 mm  Z -19..15  T2  90 ops  ~2h10                               (M5)  |
| [Rot L][Rot R][Flip H][Flip V][Move] [Outline] [Fit] [Undo]  (M6) [Use this job >]|
+--------------------------------------------------------------------------------+
```

| Control | Behaviour |
|---------|-----------|
| File line | Name, size, card and folder of the chosen file. With none chosen: "No job chosen. Tap to choose a file." Tapping it, or Change, opens the picker |
| Use this job | A file on the controller's SD: chosen, and Run opens on its start panel. A file on the display SD: uploaded first to `/sd/fluidtouch/uploads/` (D53) with a progress bar and Cancel; a file of that name already there is overwritten after a confirm ([05 §5](05-file-workflow.md#5-on-the-controller)); the upload counts once the controller lists it at the right size (S6); then Run's start panel. Greyed out while a job runs (S3) |
| Preview (M5) | Top view. Cuts coloured by depth, rapids thin and dim, stock box, job zero, probe point. Tap = zoom in at that point; Fit = whole job |
| Info line (M5) | Extents, Z range, tools, operation count, time estimate from feeds, warnings (tap for the list) |
| Rot L / Rot R (M6) | Rotate 90 CCW / CW per CNC-Tools spec 06 |
| Flip H / Flip V (M6) | Mirror per CNC-Tools spec 12. First version: "flip only", with the climb/conventional warning. Reverse-cuts later |
| Move (M6) | Enter X and Y shift in mm. Preview shows the result against the stock box |
| Blocked (M6) | If a transform can't be done faithfully: a list of line numbers and reasons; nothing changes |
| Undo (M6) | Steps back one transform (each step is a file in the working folder) |
| Outline (M7) | Switches the preview to the outline: outer edge of the cut, one loop per separate part. Shows "Run outline" in place of "Use this job" |
| Run outline (M7) | Uploads the generated outline file and opens Run's start panel for it, marked as an outline check: rapids only, no Z, raise the bit first. After it finishes, the real job is still selected |

With transforms (M6), "Use this job" uploads the working copy under a new name
(`name.r90.gcode`, `name.fh.gcode`, `name.m10x-5y.gcode`) into the same uploads folder.
Until M5 the preview area shows the file line's details in large type.

### File picker

Opens over the screen's content (800 x 360); the status bar stays visible.

```
+--------------------------------------------------------------------------------+
| [Controller SD] [Display SD]    /jobs                          [Up]   [Close]  |
|--------------------------------------------------------------------------------|
| > Clocks                                                                       |
| > Spoilboard                                                                   |
| Spoilboard_DogHoles.gcode                              789 KB          [trash] |
| Clock_Frame_A.gcode                                    212 KB          [trash] |
| ...                                                                 (scrolls)  |
+--------------------------------------------------------------------------------+
```

| Control | Behaviour |
|---------|-----------|
| Card | Controller SD (`$Files/ListGcode`, which lists G-code files and folders) or Display SD (files ending in `.gcode`, `.gc`, `.nc`, `.ngc`, `.tap`, and folders). An inbox source comes in M8 |
| List | Folders first, then files, each by name A to Z (D49). Tap a folder to open it; tap a file to choose it and close the picker. Opens in the folder last used, for the session |
| Up / Close | Parent folder / back to Job without changing the choice |
| Delete (trash) | Asks "Delete <name> from <card>?" with Delete and Cancel (D50), then deletes and refreshes the list. Controller: `$SD/Delete=<path>`. Not offered while a job runs (S3) |
| Errors | Controller offline: the controller card says so and offers the display SD. A list that fails shows the reason in place of the list |

## 4. Run

```
+------------------------------------------------+-------------------------------+
|                                                | T2 hole 12/45 X164 Y624: rough|
|                                                | op 24 of 90                   |
|            preview with progress               | [=========>        ] 27%      |
|                 ~470 x 300                     | 0:35 elapsed   ~1:35 left     |
|                                                | feed 1200 mm/min              |
|                                                |-------------------------------|
|                                                | Feed  [-10] 100% [+10] [100]  |
|                                                | Rapid [25] [50] [100]         |
|------------------------------------------------|-------------------------------|
| Spoilboard_DogHoles.gcode                      | [  PAUSE  ]      [ ABORT ]    |
+------------------------------------------------+-------------------------------+
```

States of the right-hand panel:

| State | Panel shows |
|-------|-------------|
| Start | After Job's "Use this job": file name, size and folder on the controller, and a large START (`$SD/Run=<path>`, D51). Extents and first tool are added once the analysis exists (M5) |
| Running | As drawn |
| Prompt (HOLD after `M0` with a message) | The message in large type ("Insert T2 1/8in flat endmill, attach probe") and one large CONTINUE button. Pause/Abort stay below |
| Paused (HOLD by the user) | RESUME in place of PAUSE |
| Probing | "Probing..." and the probe indicator |
| Finished / Aborted | Result, total time, [Run again] [Back to Setup]. The job's final `M0` is a Prompt like the others; CONTINUE ends the file and Finished follows (D52) |
| No job | Nothing chosen and nothing running: "No job. Choose one on the Job tab." and [Go to Job]. A job started elsewhere (classic UI, FluidNC's web UI) shows as Running |

| Control | Sends | Notes |
|---------|-------|-------|
| PAUSE / RESUME | `!` / `~` | |
| CONTINUE | `~` | Only in Prompt |
| ABORT | soft reset (0x18), then the alarm banner offers Unlock | Single press. Placed apart from PAUSE |
| Feed override | realtime override commands | [-10] value [+10] [100]; 10% steps only (D52); range 10 to 200% |
| Rapid override | realtime override commands | |
| Preview | | Cut so far in a bright colour, the rest dim, marker at the work position. Follows the tool when zoomed in |

Notes:

- The current operation text is the last `[MSG:...]` received; "op N of M" comes from the file's operation list. FluidNC sends a message when it reads the line, up to 50 s before the tool gets there (S3); from M5 the text follows the tool's line instead (D65).
- The router is switched by hand; the display does not show spindle speed or spindle override.
- An outline check uses the same screen, with a banner "Outline check" and no feed override.
- Until the preview exists (M5), the preview area shows the file name and the current
  FluidNC message in large type (D51).
- Time left: "~1:35 left" = elapsed x (100 - percent) / percent, from the controller's
  `SD:` percent (by file position, so rough). Hidden for the first minute and below 2%
  (D51). Replaced by an estimate from the file once M5 has one (D64; S3 showed how rough
  it is).

## 5. Macros

A 4 x 3 grid of 12 slots, each a button with a name and one of the classic UI's 8 macro
colours. A macro is either a file in `/sd/fluidtouch/macros/` on the controller's SD (the
classic folder, so both UIs use the same files, D13) or one command line such as `$H` or
`G0 X0 Y0` (D60). The new UI keeps its own list; it starts empty and does not read or
change classic's (D59).

```
+--------------------------------------------------------------------------------+
| Laser offset: running            [=========-------]  42%     [Resume] [ Stop ] |  strip
+-------------------+-------------------+-------------------+--------------------+
|   Laser offset    |   Park            |   Home XY         |                    |
+-------------------+-------------------+-------------------+--------------------+
|   ...             |                   |                   |                    |
+-------------------+-------------------+-------------------+--------------------+
|                   |                   |                   |                    |
+-------------------+-------------------+-------------------+--------------------+
```

**Strip** (top). With nothing running: a hint ("Tap a macro to run it." or, with no
macros, "No macros yet. Tap Edit to add one.") and Edit on the right. While a macro runs:
its name, "running", the controller's last message, a progress bar for a file (from the
`SD:` percent), Resume while the controller holds (an M0 in the macro, or a feed hold), and
Stop. Afterwards "done" or the reason it stopped, until the next macro runs or 10 s pass.
Edit is there whenever Stop is not, also beside "done" or "stopped".

**Running** (D61). A tap runs the macro when the machine is idle with no job (as Zero). A
file runs with `$SD/Run=/sd/fluidtouch/macros/<file>`; a command is sent as it is. The
macro stays on Macros: Run does not take a macro file as a job, also one started by the
classic UI or FluidNC's web UI (any file under `/fluidtouch/macros/`). While it runs the
other macros, Run's start and the moves on Setup are locked, as during a job, and the run
dot shows. Stop is a soft reset (as Abort; the controller is in alarm afterwards if it was
moving). A command counts as running while the controller is not idle after it, and done
once it is idle again (or has stayed idle for 1.5 s).

**Edit mode.** Edit turns the strip's button into Done and shows every slot, the empty ones
with a "+". Macros don't run in edit mode. Tapping a slot opens a pop-up:

- Name (keyboard pop-up, up to 23 characters).
- File or Command. File: a button with the file's name, which opens the list of files in
  `/sd/fluidtouch/macros/` (the name is filled in from the file name when it is empty).
  Command: the line (keyboard pop-up, up to 95 characters).
- Colour: the 8 swatches.
- Move left and Move right: swap with the neighbouring slot (also across rows), so the
  order can be changed.
- Clear empties the slot; Close shuts the pop-up.

Changes show at once and are written to flash, as one record, when Done is tapped or
Macros is left, and only if something changed (D44). Editing is possible offline; the file
list then says the controller is not connected.

## 6. Settings

A column of page buttons on the left (Machine, Jog & Probe, Display, Tools); the chosen
page fills the rest and scrolls when it is taller than the screen (D54).

Values are changed in place and written to flash when Settings is left: another tab, or a
restart from Tools. Only the keys that changed are written (D44, D55). Changes made and not
yet left behind are lost if the display is switched off on the Settings tab.

A number is changed by tapping it (the jog feeds also have -/+ buttons, D58): a keypad pop-up (digits, `.`, `-` where allowed,
backspace, Cancel, OK) with the allowed range under the value. A value out of range is
refused on OK with the range in red. Text (names, WiFi, address) opens a keyboard pop-up
(D56).

| Page | Contents |
|------|----------|
| Machine | Machine picker (temporary, D57): the four classic slots by name, marked "(empty)" or "(not WiFi)". For the chosen slot: name, WiFi network, WiFi password (shown as stars), FluidNC address, port. An empty slot can be chosen and filled in here; it becomes a WiFi machine with classic's jog and probe defaults. A machine with no name is called "Machine N" in messages. Saving a change of slot, WiFi or address reconnects at once (the name alone does not). Greyed out while a job runs, so the connection can't be changed under it |
| Jog & Probe | XY step at start and Z step at start: the step lists as buttons; the one tapped is the step Setup starts with. The lists themselves are not changed here (D58). XY jog feed, Z jog feed, joystick feed at full deflection, each with - and + buttons beside the value that change it by 100 mm/min; tapping the value opens the keypad. Probe feed, probe maximum distance, probe back-off. Laser offset X and Y (laser crosshair to bit, D11). Setup picks up new values the next time it is shown; the step chosen there goes back to the start step |
| Display | Brightness: a slider from 25 to 100% in steps of 5. Dimming goes to a quarter of the brightness, at least 5% (stored in classic's `pm_dim_bri`; classic's own Power page rounds both up to its choices) (D58). Dim after: off, 15, 30, 45 s, 1 min, 90 s, 2, 5 min. Screen off after: off, 1, 2, 5, 10, 15, 30, 60 min. Deep sleep after: off, 5, 10, 15, 30, 60, 90 min. Rotate 180: off/on. Same keys and choices as classic Settings > Power and General. Brightness changes while the slider moves; the timers follow classic's rules (screen off at least 10 s after dim, deep sleep at least 60 s after screen off), and a choice that breaks them moves the later timer, and later choices that would break them are greyed out. Rotation takes effect after a restart, which the page says |
| Tools | Restart into classic UI, Restart, Draw test (spike S2, goes with M5). Position log: Off / On (D62), with the last log's file name and rows, or why it stopped. About: LowRider Touch and FluidTouch versions, FluidNC address and version, display IP and WiFi signal, free RAM and PSRAM, time since start |

The spec's earlier Files page (file manager, copy to controller, inbox address) is not
built: the Job picker lists and deletes on both cards, "Use this job" copies to the
controller, and the inbox address comes with M8. Travel X/Y/Z comes with the Move screen,
which is the first thing to use it. Terminal and settings backup/restore are later (M8).
