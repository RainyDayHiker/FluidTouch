# 03 — Screens

First draft. Each screen has a wireframe, a control table, and questions. Sizes are in
pixels on the 800 x 480 display. Touch targets are at least 64 x 56.

## 1. Frame

```
+--------------------------------------------------------------------------------+
|   Setup    |     Job     |     Run     |    Macros    |   Settings             | 56
+--------------------------------------------------------------------------------+
|                                                                                |
|                              screen content  800 x 376                         |
|                                                                                |
+--------------------------------------------------------------------------------+
| IDLE  W X123.45 Y67.89 Z15.00   M X223.45 Y167.89 Z-5.00   wifi   [  STOP  ]   | 48
+--------------------------------------------------------------------------------+
```

Screen tabs are at the top; the status bar is at the bottom (D27).

| Element | Behaviour |
|---------|-----------|
| State | IDLE, RUN, HOLD, JOG, HOME, ALARM, OFFLINE; colour coded |
| Position | Work position and machine position, both always visible, in the status bar |
| STOP | Jogging: jog cancel. Otherwise feed hold (`!`), which FluidNC ignores when idle. Always enabled. Resume is only on the Run screen, so it can't be hit by habit. The button is 104 x 40 because the bar is 48 high; its touch area extends 10 px beyond it on every side (64 x 60 effective). Checked on the device: good |
| Alarm banner | Replaces the status bar: alarm text + Unlock (`$X`). On every screen |
| Offline banner | Replaces the status bar. The text says whether an attempt is under way or when the next one starts. During an attempt: "Connecting to <machine>..." before the first connection, "Connection lost. Reconnecting..." after a drop; Reconnect is greyed out, because it could do nothing. Between attempts: "No answer from <machine>. Next attempt in N s." or "Connection lost. Next attempt in N s.", counting down; Reconnect starts the attempt at once. While joining WiFi: "Connecting to WiFi <ssid>..." (or "WiFi lost. Reconnecting to <ssid>..."). If a job was running when the link dropped, a second line "The job continues on the controller." Retries happen on their own ([02 §4](02-architecture.md#4-services)) |
| No-machine banner | Classic machine slot 1 is empty or not a WiFi machine: says so, with a Classic UI button (restart into classic to set it up). Until Settings > Machine exists (M3) |
| Navigation | Always available. A dot on Run while a job is active. Starting a job switches to Run |

## 2. Setup

Laid out left to right in the order of a session: power on, **home all**, **zero all**, add
stock, **jog** to the start point (with the laser), **start**.

```
+--------------------------------------------------------------------------------+
| [Home all] [Zero all]  |  [Home Z] [Zero XY] [Zero Z]  |  [Unlock] [Reset]     |
|                                                         lim X Y Z   probe o    |
+-----------------------------+----------+---------------------------------------+
| XY [100][50][10][1][0.1]    | Z [50][25][10][1][0.1]                           |  [ Laser -> bit ]   [ Bit -> laser ]  |
|                             |   [Z+]   |  [ Go to XY zero ]  [ Probe Z ... ]   |
|   [\] [Y+] [/]              |          |                                       |
|   [X-] [  ] [X+]            |   [Z-]   |  Job: Spoilboard_DogHoles.gcode       |
|   [/] [Y-] [\]              |          |  520 x 864 mm   fits from here: yes   |
|  feed: [-] 2400 [+]         |          |  [ Choose job ]      [ Start > ]      |
+-----------------------------+----------+---------------------------------------+
```

| Control | Sends | Notes |
|---------|-------|-------|
| Home all / Home Z | `$H` / `$HZ` | Single press |
| Zero all / Zero XY / Zero Z | `G10 L20 P0 X0 Y0 Z0` / `... X0 Y0` / `... Z0` | Zero all is the everyday one; the other two are smaller |
| Unlock | `$X` | Highlighted in ALARM |
| Reset | soft reset (0x18) | Kept at the far right, away from the jog pad |
| Jog pad | `$J=G91 G21 X.. F..` | Step from the XY selector |
| Z+/Z- | `$J=G91 G21 Z.. F..` | Own step selector above the Z buttons, own stored feed |
| Step selectors | | XY: 100, 50, 10, 1, 0.1 mm. Z: 50, 25, 10, 1, 0.1 mm. Editable in Settings; last used remembered per axis group |
| Laser -> bit | `$J=G91 G21 X33.6 Y-28.2 F..` | Offset from Settings (defaults from `laser_offset.gcode`). A jog, so it can be cancelled and leaves G90/G91 alone |
| Bit -> laser | The reverse move | |
| Go to XY zero | `$J=G90 G21 X0 Y0 F..` | STOP cancels it. No Z height check |
| Probe Z ... | Opens a panel: feed, max distance (from Settings), [Probe], result line | Reports the probed Z only: it does not set zero or retract. For levelling, squaring and test work; jobs probe themselves |
| Job strip | | Selected job, extents, and whether they fit machine travel from the current position |
| Start | | Opens Run's start panel. Enabled when a job is selected and state is IDLE |

Not carried over (still in the classic UI): jog to typed coordinates, joystick, X/Y probing.

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
