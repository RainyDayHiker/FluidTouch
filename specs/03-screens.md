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
| No-machine banner | Classic machine slot 1 is empty or not a WiFi machine: says so, with a Classic UI button (restart into classic to set it up). Until Settings > Machine exists (M3) |
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

```
+---------------------------+----------------------------------------------------+
| Source: [Inbox v]  [Refr] |                                                    |
|---------------------------|                                                    |
| Spoilboard_DogHoles  789K |                 preview  ~500 x 290                |
| Clock_Frame_A        212K |                                                    |
| ...                       |                                                    |
|                           |----------------------------------------------------|
|                           | 520 x 864 mm  Z -19..15  T2  90 ops  ~2h10         |
|                           | [Rot L][Rot R][Flip H][Flip V][Move] [Outline] [Fit]|
|                           | [ Undo ]                         [ Use this job ]  |
+---------------------------+----------------------------------------------------+
```

| Control | Behaviour |
|---------|-----------|
| Source | Where the list comes from: the inbox ([05](05-file-workflow.md)), FluidNC SD, display SD |
| File list | Newest first. Tap = fetch to the working folder and analyse; progress shown in the preview area |
| Preview | Top view. Cuts coloured by depth, rapids thin and dim, stock box, job zero, probe point. Tap = zoom in at that point; Fit = whole job |
| Info line | Extents, Z range, tools, operation count, time estimate from feeds, warnings (tap for the list) |
| Rot L / Rot R | Rotate 90 CCW / CW per CNC-Tools spec 06 |
| Flip H / Flip V | Mirror per CNC-Tools spec 12. First version: "flip only", with the climb/conventional warning. Reverse-cuts later |
| Move | Enter X and Y shift in mm. Preview shows the result against the stock box |
| Blocked | If a transform can't be done faithfully: a list of line numbers and reasons; nothing changes |
| Undo | Steps back one transform (each step is a file in the working folder) |
| Outline | Switches the preview to the outline: outer edge of the cut, one loop per separate part. Shows "Run outline" in place of "Use this job" |
| Run outline | Uploads the generated outline file and opens Run's start panel for it, marked as an outline check: rapids only, no Z, raise the bit first. After it finishes, the real job is still selected |
| Use this job | Untransformed and already on the controller: select it. Otherwise upload the working copy under a new name (`name.r90.gcode`, `name.fh.gcode`, `name.m10x-5y.gcode`) and select that |

Questions:

- **Q14** Time estimate: wanted, even if rough (it ignores acceleration)?
- **Q15** In the preview, would you rather colour by depth or by operation/tool?
- **Q16** For Move, are typed X/Y values enough, or do you want nudge buttons (1, 5, 10 mm)?

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
| Start | File, extents, first tool, "Zero will be set at the current position", START button |
| Running | As drawn |
| Prompt (HOLD after `M0` with a message) | The message in large type ("Insert T2 1/8in flat endmill, attach probe") and one large CONTINUE button. Pause/Abort stay below |
| Paused (HOLD by the user) | RESUME in place of PAUSE |
| Probing | "Probing..." and the probe indicator |
| Finished / Aborted | Result, total time, [Run again] [Back to Setup] |

| Control | Sends | Notes |
|---------|-------|-------|
| PAUSE / RESUME | `!` / `~` | |
| CONTINUE | `~` | Only in Prompt |
| ABORT | soft reset (0x18), then the alarm banner offers Unlock | Single press. Placed apart from PAUSE |
| Feed override | realtime override commands | Range 10 to 200% |
| Rapid override | realtime override commands | |
| Preview | | Cut so far in a bright colour, the rest dim, marker at the work position. Follows the tool when zoomed in |

Notes:

- The current operation text is the last `[MSG:...]` received; "op N of M" comes from the file's operation list.
- The router is switched by hand; the display does not show spindle speed or spindle override.
- An outline check uses the same screen, with a banner "Outline check" and no feed override.

Questions:

- **Q18** After the job's final `M0 (MSG Job complete - stop router)`, is CONTINUE then "Finished" the right ending?
- **Q19** Feed override steps: 10% only, or 1% as well?

## 5. Macros

A 4 x 3 grid of buttons (name, colour), as in the classic UI but with 12 slots. A macro is
a file in `/sd/fluidtouch/macros/` (same folder as now, so both UIs share them) or a single
command line. Edit mode: name, file or command, colour, order. A running macro shows its
last message and a Stop.

## 6. Settings

| Page | Contents |
|------|----------|
| Machine | WiFi SSID and password, FluidNC address and port, travel X/Y/Z |
| Jog | XY and Z step lists, XY and Z feeds |
| Laser | Offset X and Y |
| Files | File manager for all storages (list, delete, copy display SD -> controller); inbox address |
| Display | Brightness, dim/sleep timers, rotate 180 |
| Tools | Terminal, backup/restore settings, restart into classic UI, about (versions, free memory) |
