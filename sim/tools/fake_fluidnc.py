#!/usr/bin/env python3
"""Minimal fake FluidNC WebSocket server for testing the FluidTouch simulator
without a real controller. Standard library only (Python 3.8+).

    python sim/tools/fake_fluidnc.py [--port 81] [--sd <folder>]

Then add a machine in the simulator with URL 127.0.0.1 and port 81.

It speaks just enough of the FluidNC websocket protocol for FluidTouch:
status reports (polled "?" or $Report/Interval auto-reports), $G parser
state, jogging ($J=), homing ($H/$HX..), zeroing (G10 L20), feed hold /
resume / reset, overrides, unlock, $Files/ListGcode and $SD/Delete (on the
folder given with --sd, or a few sample files). Anything else gets "ok".

It also runs jobs: $SD/Run=<path> (or $LocalFS/Run=) plays a G-code file from
the --sd folder, or a built-in sample job when no folder is given. The
reported position follows the path (G0-G3, G90/G91, G53, G10 L20, G4) at the
programmed feed rate, with feed and rapid overrides applied. Status reports
carry SD:<percent>,<path>, where the percent is how far FluidNC has *read*
the file: up to 15 lines ahead of the motion, stopping at lines that wait for
motion to finish (M0, G4, G10, G38), and gone once the last line is read.
(MSG text) comments are sent as [MSG:INFO: MSG,text] when their line is read,
M0 holds until cycle start (~), G38.2 probes (contact at 60% of the travel)
and sends [PRB:x,y,z:1], feed hold pauses, and a reset aborts (ALARM:3 if it
was moving). Motion lines sent directly are played the same way, so a probe
can be tried with e.g. "G38.2 Z-10 F100"; lines sent while one runs queue
behind it.

HTTP is served on the same port, as FluidNC 4 does: GET /sd/<path> downloads a
file, /upload lists, creates, deletes and takes multipart uploads for the SD
card, /files does the same for the flash filesystem. The classic UI uploads
to port 80 whatever the websocket port is, so use --port 80 or --http-port 80
to try its uploads.

It also plays FluidNC's side of ESP-NOW (v4.0.4+) on UDP port 8182, for the
simulator's ESP-NOW radio: $espnow/pair, $espnow/list, $espnow/unpair=<n> and
$espnow/cancel work as on FluidNC, and --espnow-pair opens the pairing window
at startup. Pairings are remembered in sim/data/fake_espnow_pairings.json.

Test helpers (type them in the simulator's Terminal tab or console, or in
this server's own console, which passes each line to every connected client's
session as if that client had sent it):
    $sim/alarm=<n>   raise ALARM:<n>
    $sim/msg=<text>  send [MSG:<text>]
    $sim/limit=XYZA  set limit pins (e.g. $sim/limit=X, or $sim/limit= to clear)
    $sim/probe=1|0   set probe pin
    $sim/state=Door:0  report any state string (e.g. Door:0, Sleep, Bogus)
    $sim/speed=<n>   play jobs n times faster (also --speed)
    $sim/probemiss=1|0  G38.2 finds nothing: [PRB:...:0] and ALARM:5
    $sim/drop=<s>    close every connection and refuse new ones for s seconds
                     (a running job continues)
    $sim/httprate=<n>  limit HTTP transfers to n KB/s (0 = no limit)
    $sim/httpblock=1|0  refuse downloads with 503 while the machine moves
"""

import argparse
import asyncio
import base64
import hashlib
import hmac
import json
import math
import os
import re
import secrets
import shutil
import struct
import sys
import tempfile
import threading
import time
import urllib.parse

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
AXES = "XYZA"


class Machine:
    def __init__(self):
        self.state = "Idle"
        self.mpos = [0.0, 0.0, 0.0, 0.0]
        self.wco = [0.0, 0.0, 0.0, 0.0]
        self.feed_ov = 100
        self.rapid_ov = 100
        self.spindle_ov = 100
        self.limits = ""
        self.probe = False
        self.busy_until = 0.0
        # Job playback (see run_job)
        self.job = None            # Job being played, also for a single direct line
        self.task = None
        self.direct_lines = []     # G-code lines sent directly, waiting to be played
        self.direct_running = False
        self.holding = False       # feed hold or M0, until cycle start
        self.abort = False
        self.absolute = True       # G90 / G91
        self.inches = False        # G20 / G21
        self.motion = 0            # modal G0..G3
        self.feed = 1000.0         # mm/min
        self.feed_now = 0.0        # reported in FS:
        self.speed = 1.0           # playback speed-up, for tests
        self.probe_miss = False
        self.probe_hit_until = 0.0
        self.drop_until = 0.0
        self.http_rate = 0         # KB/s limit for HTTP transfers, 0 = none
        self.http_block = False    # refuse downloads while moving
        self.broadcast = lambda line: None  # send a line to every connected client

    def tick(self):
        if self.state in ("Jog", "Home") and time.monotonic() >= self.busy_until:
            self.state = "Idle"

    def status(self):
        self.tick()
        mpos = ",".join(f"{v:.3f}" for v in self.mpos)
        wco = ",".join(f"{v:.3f}" for v in self.wco)
        probe = self.probe or time.monotonic() < self.probe_hit_until
        pins = self.limits + ("P" if probe else "")
        report = (f"<{self.state}|MPos:{mpos}|FS:{self.feed_now:.0f},0|WCO:{wco}"
                  f"|Ov:{self.feed_ov},{self.rapid_ov},{self.spindle_ov}")
        if pins:
            report += f"|Pn:{pins}"
        sd = self.job.sd_field() if self.job else None
        if sd:
            report += "|" + sd
        return report + ">"


    def busy(self):
        return self.task is not None and not self.task.done()


# ---------------------------------------------------------------------------
# Job playback
# ---------------------------------------------------------------------------

RAPID_RATE = 5000.0  # mm/min for G0
LOOKAHEAD = 15       # lines FluidNC has read beyond the one in motion
TICK = 0.05          # seconds between position updates

COMMENT_RE = re.compile(r"\(([^)]*)\)")
WORD_RE = re.compile(r"([A-Z])\s*([-+]?(?:\d+\.?\d*|\.\d+))")
# Lines FluidNC finishes all queued motion for before reading on
SYNC_RE = re.compile(r"(?<![A-Z0-9.])(M0?[012]|M30|G0?4|G10|G38(?:\.\d)?)(?![0-9.])")

def make_sample_job():
    """Played by $SD/Run when no --sd folder is given. The shape of a LowRider
    job: zero, bit prompt, probe, operations announced with (MSG ...), final
    prompt. About 150 lines, so the SD: percent moves in small steps."""
    g = ["(Sample job for the fake FluidNC)", "(Stock: 300 x 200 x 18)", "G21 G90",
         "G10 L20 P0 X0 Y0 Z0",
         "M0 (MSG Insert 1/4 in bit and attach probe)",
         "G38.2 Z-20 F200", "G10 L20 P0 Z0.5", "G0 Z10",
         "M0 (MSG Remove probe - start router)",
         "(MSG Operation 1 of 2: Pocket)", "G0 X152 Y100"]
    for depth in (-3, -6):
        g.append(f"G1 Z{depth} F300")
        for ring in range(1, 16):
            r = ring * 2
            g += [f"G1 X{150 + r} Y100 F1500", f"G2 X{150 - r} Y100 I-{r} J0", f"G2 X{150 + r} Y100 I{r} J0"]
        g.append("G0 Z2")
        g.append("G0 X152 Y100")
    g += ["G0 Z10", "G4 P1.5", "(MSG Operation 2 of 2: Profile)", "G0 X10 Y10"]
    for depth in (-4, -8, -12, -16, -18.5):
        g += [f"G1 Z{depth} F300", "G1 X280 Y10 F2500", "G3 X290 Y20 I0 J10", "G1 X290 Y180",
              "G3 X280 Y190 I-10 J0", "G1 X20 Y190", "G3 X10 Y180 I0 J-10", "G1 X10 Y20",
              "G3 X20 Y10 I10 J0", "G1 X10 Y10"]
    g += ["G0 Z10", "G0 X0 Y0", "M0 (MSG Job complete - stop router)"]
    return ("\n".join(g) + "\n").encode()


SAMPLE_JOB = make_sample_job()


def strip_comments(line):
    return COMMENT_RE.sub("", line).split(";", 1)[0]


def is_sync_line(line):
    return SYNC_RE.search(strip_comments(line).upper().replace(" ", "")) is not None


class Job:
    """A file being played (path set) or a single line sent directly (path None)."""

    def __init__(self, data: bytes, path=None):
        self.path = path
        self.size = max(1, len(data))
        self.lines, self.ends = [], []
        pos = 0
        for raw in data.splitlines(keepends=True):
            pos += len(raw)
            self.lines.append(raw.decode(errors="replace").strip())
            self.ends.append(pos)
        n = len(self.lines)
        self.read_idx = -1  # last line FluidNC has read
        self.next_sync = [n] * (n + 1)
        for i in range(n - 1, -1, -1):
            self.next_sync[i] = i if is_sync_line(self.lines[i]) else self.next_sync[i + 1]

    def read_limit(self, exec_idx):
        return min(exec_idx + LOOKAHEAD, self.next_sync[exec_idx], len(self.lines) - 1)

    def sd_field(self):
        # FluidNC drops the SD: field once it has read the last line, even
        # though the motion queued from those lines is still running
        if self.path is None or self.read_idx >= len(self.lines) - 1:
            return None
        pos = self.ends[self.read_idx] if self.read_idx >= 0 else 0
        return f"SD:{pos * 100.0 / self.size:.2f},{self.path}"


async def run_job(m, job):
    m.job = job
    m.abort = False
    m.holding = False
    try:
        for i, line in enumerate(job.lines):
            limit = job.read_limit(i)
            while job.read_idx < limit:
                job.read_idx += 1
                # Comments are handled when a line is read, ahead of the motion
                for text in COMMENT_RE.findall(job.lines[job.read_idx]):
                    if "MSG" in text:
                        m.broadcast(f"[MSG:INFO: MSG,{text[4:]}]")
            await execute_line(m, line)
            if m.abort:
                return
    finally:
        m.job = None
        m.feed_now = 0.0
        m.holding = False
        if m.state != "Alarm":
            m.state = "Idle"


async def execute_line(m, raw):
    code = strip_comments(raw).upper().strip()
    if not code or code[0] in "%$":
        return
    words = WORD_RE.findall(code)
    gs = [float(v) for letter, v in words if letter == "G"]
    ms = [int(float(v)) for letter, v in words if letter == "M"]
    vals = {letter: float(v) for letter, v in words if letter not in "GMN"}

    for g in gs:
        if g == 20: m.inches = True
        elif g == 21: m.inches = False
        elif g == 90: m.absolute = True
        elif g == 91: m.absolute = False
        elif g in (0, 1, 2, 3): m.motion = int(g)
    unit = 25.4 if m.inches else 1.0
    if "F" in vals:
        m.feed = vals["F"] * unit

    if 4 in gs:
        # FluidNC reports Idle during a dwell, with the SD: field still there
        m.state, m.feed_now = "Idle", 0.0
        end = time.monotonic() + vals.get("P", 0.0) / m.speed
        while time.monotonic() < end and not m.abort:
            await asyncio.sleep(TICK)
        return
    if 10 in gs:
        level = int(vals.get("L", 20))
        for i, axis in enumerate(AXES):
            if axis in vals:
                m.wco[i] = m.mpos[i] - vals[axis] * unit if level == 20 else vals[axis] * unit
        return
    if 92 in gs:
        return

    probing = any(38 <= g < 39 for g in gs)
    if probing or any(axis in vals for axis in AXES):
        target = list(m.mpos)
        for i, axis in enumerate(AXES):
            if axis in vals:
                v = vals[axis] * unit
                target[i] = v if 53 in gs else (v + m.wco[i] if m.absolute else m.mpos[i] + v)
        if probing:
            await probe_move(m, target)
        elif m.motion in (2, 3):
            await arc_move(m, target, vals, unit, clockwise=m.motion == 2)
        else:
            await linear_move(m, target, rapid=m.motion == 0)
        if m.abort:
            return

    for code_m in ms:
        if code_m in (0, 1):
            m.holding = True
            while m.holding and not m.abort:
                m.state, m.feed_now = "Hold:0", 0.0
                await asyncio.sleep(TICK)


async def move(m, length, set_position, rapid):
    """Advances along a path of `length` mm; set_position(fraction) places the tool."""
    done, last = 0.0, time.monotonic()
    while True:
        if m.abort:
            return
        if m.holding:
            m.state, m.feed_now = "Hold:0", 0.0
            await asyncio.sleep(TICK)
            last = time.monotonic()
            continue
        m.state = "Run"
        m.feed_now = RAPID_RATE * m.rapid_ov / 100 if rapid else m.feed * m.feed_ov / 100
        now = time.monotonic()
        done += m.feed_now * m.speed / 60.0 * (now - last)
        last = now
        if length <= 0 or done >= length:
            set_position(1.0)
            return
        set_position(done / length)
        await asyncio.sleep(TICK)


async def linear_move(m, target, rapid):
    start = list(m.mpos)
    length = math.sqrt(sum((target[i] - start[i]) ** 2 for i in range(3))) or abs(target[3] - start[3])

    def set_position(f):
        m.mpos = [start[i] + (target[i] - start[i]) * f for i in range(4)]

    await move(m, length, set_position, rapid)


async def arc_move(m, target, vals, unit, clockwise):
    start = list(m.mpos)
    if "R" in vals:
        # Centre from the radius: the minor arc for R > 0
        r = abs(vals["R"]) * unit
        dx, dy = target[0] - start[0], target[1] - start[1]
        d = math.hypot(dx, dy)
        if d == 0:
            return
        h = math.sqrt(max(0.0, r * r - d * d / 4))
        side = (-1 if clockwise else 1) * (1 if vals["R"] >= 0 else -1)
        cx = (start[0] + target[0]) / 2 - side * h * dy / d
        cy = (start[1] + target[1]) / 2 + side * h * dx / d
    else:
        cx = start[0] + vals.get("I", 0.0) * unit
        cy = start[1] + vals.get("J", 0.0) * unit
    r = math.hypot(start[0] - cx, start[1] - cy)
    a0 = math.atan2(start[1] - cy, start[0] - cx)
    sweep = math.atan2(target[1] - cy, target[0] - cx) - a0
    if clockwise and sweep >= 0:
        sweep -= 2 * math.pi
    elif not clockwise and sweep <= 0:
        sweep += 2 * math.pi
    length = math.hypot(r * abs(sweep), target[2] - start[2])

    def set_position(f):
        if f >= 1.0:
            m.mpos = list(target)
            return
        a = a0 + sweep * f
        m.mpos = [cx + r * math.cos(a), cy + r * math.sin(a),
                  start[2] + (target[2] - start[2]) * f, start[3]]

    await move(m, length, set_position, rapid=False)


async def probe_move(m, target):
    start = list(m.mpos)
    # Contact at once if the pin is already set, else at 60% of the travel
    fraction = 0.0 if m.probe else (1.0 if m.probe_miss else 0.6)
    stop = [start[i] + (target[i] - start[i]) * fraction for i in range(4)]
    await linear_move(m, stop, rapid=False)
    if m.abort:
        return
    hit = m.probe or not m.probe_miss
    position = ",".join(f"{v:.3f}" for v in m.mpos[:3])
    m.broadcast(f"[PRB:{position}:{1 if hit else 0}]")
    if hit:
        m.probe_hit_until = time.monotonic() + 0.5
    else:
        m.state, m.abort = "Alarm", True
        m.broadcast("ALARM:5")


def start_job(m, data: bytes, path=None):
    if m.busy():
        return False
    m.task = asyncio.get_event_loop().create_task(run_job(m, Job(data, path)))
    return True


def queue_direct_line(m, line):
    m.direct_lines.append(line)
    if not m.direct_running:
        m.direct_running = True
        m.task = asyncio.get_event_loop().create_task(run_direct_lines(m))


async def run_direct_lines(m):
    """Plays queued direct lines one after the other; an alarm or a reset drops the rest."""
    try:
        while m.direct_lines and m.state != "Alarm":
            await run_job(m, Job(m.direct_lines.pop(0).encode()))
            if m.abort:
                break
    finally:
        m.direct_lines.clear()
        m.direct_running = False


def load_job_file(sd_root, path):
    """-> file contents for $SD/Run, or None if there is no such file."""
    if sd_root is None:
        return SAMPLE_MACROS.get(path, SAMPLE_JOB)
    rel = path[len("/sd/"):] if path.startswith("/sd/") else path.lstrip("/")
    full = os.path.realpath(os.path.join(sd_root, rel))
    if not full.startswith(os.path.realpath(sd_root)) or not os.path.isfile(full):
        return None
    with open(full, "rb") as f:
        return f.read()


async def read_frame(reader):
    """Read one client frame -> (opcode, payload bytes)."""
    b1, b2 = await reader.readexactly(2)
    opcode = b1 & 0x0F
    length = b2 & 0x7F
    if length == 126:
        (length,) = struct.unpack(">H", await reader.readexactly(2))
    elif length == 127:
        (length,) = struct.unpack(">Q", await reader.readexactly(8))
    mask = await reader.readexactly(4) if b2 & 0x80 else b"\0\0\0\0"
    data = bytearray(await reader.readexactly(length))
    for i in range(length):
        data[i] ^= mask[i % 4]
    return opcode, bytes(data)


def make_frame(payload: bytes, opcode=0x2):
    header = bytes([0x80 | opcode])
    n = len(payload)
    if n < 126:
        header += bytes([n])
    elif n < 65536:
        header += bytes([126]) + struct.pack(">H", n)
    else:
        header += bytes([127]) + struct.pack(">Q", n)
    return header + payload


# Macro files in the classic UI's folder, with their contents. wait_here.gcode
# holds at an M0, to try Resume.
SAMPLE_MACROS = {
    "/sd/fluidtouch/macros/laser_offset.gcode": b"G91\nG0 X33.6 Y-28.2\nG90\n",
    "/sd/fluidtouch/macros/park.gcode": b"G90\nG0 Z10\nG0 X0 Y0\n",
    "/sd/fluidtouch/macros/wait_here.gcode": b"G91\nG0 X5\nM0 (MSG Check the bit, then Resume)\nG0 X-5\nG90\n",
}

SAMPLE_FILES = {"/sd/": [("fluidtouch", -1), ("projects", -1), ("facing.nc", 18432), ("sign_v2.gcode", 254871)],
                "/sd/projects/": [("bracket.nc", 9811), ("drawer_front.nc", 120334)],
                "/sd/fluidtouch/": [("macros", -1)],
                "/sd/fluidtouch/macros/": [(p.rpartition("/")[2], len(d)) for p, d in SAMPLE_MACROS.items()]}


def list_files(sd_root, path):
    """-> (files list of {"name","size"}, error or None) for $Files/ListGcode."""
    if not path.endswith("/"):
        path += "/"
    if sd_root is None:
        entries = SAMPLE_FILES.get(path)
        if entries is None:
            return [], "Not a directory"
        return [{"name": n, "size": str(sz)} for n, sz in entries], None
    rel = path[len("/sd/"):] if path.startswith("/sd/") else path.lstrip("/")
    folder = os.path.join(sd_root, rel)
    if not os.path.isdir(folder):
        return [], "Not a directory"
    files = []
    for name in sorted(os.listdir(folder)):
        full = os.path.join(folder, name)
        files.append({"name": name, "size": "-1" if os.path.isdir(full) else str(os.path.getsize(full))})
    return files, None


def delete_file(sd_root, path):
    """$SD/Delete=<path> (a file, not a folder) -> error text or None."""
    if sd_root is None:
        folder, _, name = path.rpartition("/")
        entries = SAMPLE_FILES.get(folder + "/", [])
        for entry in entries:
            if entry[0] == name and entry[1] != -1:
                entries.remove(entry)
                return None
        return "No such file"
    rel = path[len("/sd/"):] if path.startswith("/sd/") else path.lstrip("/")
    full = os.path.realpath(os.path.join(sd_root, rel))
    if not full.startswith(os.path.realpath(sd_root)) or not os.path.isfile(full):
        return "No such file"
    try:
        os.remove(full)
    except OSError:
        return "Delete failed"
    return None


class Session:
    """One client channel. `out` takes controller output bytes (newline-terminated lines)."""

    def __init__(self, out, machine, sd_root=None, report_interval=0):
        self.out, self.m = out, machine
        self.sd_root = sd_root
        self.report_interval = report_interval

    def send(self, line: str):
        self.out((line + "\n").encode())

    async def auto_report(self):
        while True:
            if self.report_interval:
                self.send(self.m.status())
                await asyncio.sleep(self.report_interval / 1000)
            else:
                await asyncio.sleep(0.1)

    def handle_input(self, data: bytes):
        # Single-byte realtime commands arrive as their own message
        if len(data) == 1 and self.handle_realtime(data[0]):
            return
        for line in data.decode(errors="replace").replace("\r", "").split("\n"):
            if len(line) == 1 and self.handle_realtime(ord(line)):
                continue
            if line:
                print(f"  <- {line}")
            self.handle_line(line.strip())

    def handle_realtime(self, ch: int) -> bool:
        m = self.m
        if ch == ord("?"):
            self.send(m.status())
        elif ch == ord("!"):
            # As on FluidNC: feed hold ends a jog and is ignored when idle
            if m.state == "Jog":
                m.state = "Idle"
            elif m.busy() and m.state != "Alarm":
                m.holding = True
            elif m.state == "Run":
                m.state = "Hold:0"
        elif ch == ord("~"):
            if m.busy():
                m.holding = False
            elif m.state.startswith("Hold"):
                m.state = "Idle"
        elif ch == 0x18:
            # As on FluidNC: a reset while moving loses position (ALARM:3);
            # from a hold, or when idle, it does not
            moving = m.busy() and m.state == "Run"
            if m.busy():
                m.abort = True
            m.state = "Alarm" if moving else "Idle"
            if moving:
                m.broadcast("ALARM:3")
            self.send("Grbl 3.9 [FluidNC fake (sim) '$' for help]")
        elif ch == 0x85:
            if m.state == "Jog":
                m.state = "Idle"
        elif 0x90 <= ch <= 0x9D:
            if ch == 0x90: m.feed_ov = 100
            elif ch == 0x91: m.feed_ov = min(200, m.feed_ov + 10)
            elif ch == 0x92: m.feed_ov = max(10, m.feed_ov - 10)
            elif ch == 0x93: m.feed_ov = min(200, m.feed_ov + 1)
            elif ch == 0x94: m.feed_ov = max(10, m.feed_ov - 1)
            elif ch == 0x95: m.rapid_ov = 100
            elif ch == 0x96: m.rapid_ov = 50
            elif ch == 0x97: m.rapid_ov = 25
            elif ch == 0x99: m.spindle_ov = 100
            elif ch == 0x9A: m.spindle_ov = min(200, m.spindle_ov + 10)
            elif ch == 0x9B: m.spindle_ov = max(10, m.spindle_ov - 10)
            elif ch == 0x9C: m.spindle_ov = min(200, m.spindle_ov + 1)
            elif ch == 0x9D: m.spindle_ov = max(10, m.spindle_ov - 1)
        else:
            return False
        return True

    def handle_line(self, line: str):
        m = self.m
        up = line.upper()
        if not line:
            return
        if up.startswith("$REPORT/INTERVAL="):
            self.report_interval = int(line.split("=", 1)[1] or 0)
            self.send(f"[MSG:INFO: Report/Interval={self.report_interval}]")
            self.send("ok")
        elif up == "$G":
            self.send("[GC:G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0]")
            self.send("ok")
        elif up == "$X":
            m.state = "Idle"
            self.send("[MSG:Caution: Unlocked]")
            self.send("ok")
        elif up.startswith("$H"):
            axes = up[2:] or AXES
            for a in axes:
                if a in AXES:
                    m.mpos[AXES.index(a)] = 0.0
            m.state = "Home"
            m.busy_until = time.monotonic() + 1.5
            self.send("ok")
        elif up.startswith("$J="):
            relative = "G91" in up
            for axis, value in re.findall(r"([XYZA])(-?[\d.]+)", up[3:]):
                i = AXES.index(axis)
                v = float(value)
                m.mpos[i] = m.mpos[i] + v if relative else v + (m.wco[i] if "G53" not in up else 0)
            m.state = "Jog"
            m.busy_until = time.monotonic() + 0.5
            self.send("ok")
        elif up.startswith("G10") and "L20" in up:
            for axis, value in re.findall(r"([XYZA])(-?[\d.]+)", up):
                i = AXES.index(axis)
                m.wco[i] = m.mpos[i] - float(value)
            self.send("ok")
        elif up.startswith("$FILES/LISTGCODE"):
            path = line.split("=", 1)[1].strip() if "=" in line else "/sd/"
            files, error = list_files(self.sd_root, path or "/sd/")
            reply = {"error": error} if error else {"files": files, "path": path}
            self.send("[JSON:" + json.dumps(reply, separators=(",", ":")) + "]")
            self.send("ok")
        elif up.startswith("$SD/DELETE="):
            path = line.split("=", 1)[1].strip()
            error = delete_file(self.sd_root, path)
            if error:
                self.send("[MSG:ERR: " + error + " " + path + "]")
                self.send("error:65" if error == "No such file" else "error:69")
            else:
                print(f"  deleted {path}")
                self.send("ok")
        elif up.startswith("$SD/RUN=") or up.startswith("$LOCALFS/RUN="):
            path = line.split("=", 1)[1].strip()
            data = load_job_file(self.sd_root, path)
            if data is None:
                self.send("[MSG:ERR: Cannot open file " + path + "]")
                self.send("error:65")
            elif not start_job(m, data, path):
                self.send("error:67")  # busy
            else:
                print(f"  running {path}")
                self.send("ok")
        elif up.startswith("$SIM/SPEED="):
            m.speed = max(0.01, float(line.split("=", 1)[1] or 1))
            self.send("ok")
        elif up.startswith("$SIM/PROBEMISS="):
            m.probe_miss = line.split("=", 1)[1].strip() == "1"
            self.send("ok")
        elif up.startswith("$SIM/HTTPRATE="):
            m.http_rate = max(0.0, float(line.split("=", 1)[1] or 0))
            self.send("ok")
        elif up.startswith("$SIM/HTTPBLOCK="):
            m.http_block = line.split("=", 1)[1].strip() == "1"
            self.send("ok")
        elif up.startswith("$SIM/DROP="):
            m.drop_until = time.monotonic() + float(line.split("=", 1)[1] or 5)
            for writer in list(writers):
                writer.close()
        elif up.startswith("$SIM/ALARM="):
            m.state = "Alarm"
            self.send(f"ALARM:{line.split('=', 1)[1]}")
        elif up.startswith("$SIM/STATE="):
            m.state = line.split("=", 1)[1].strip() or "Idle"
            self.send("ok")
        elif up.startswith("$SIM/MSG="):
            self.send(f"[MSG:{line.split('=', 1)[1]}]")
        elif up.startswith("$SIM/LIMIT="):
            m.limits = "".join(a for a in line.split("=", 1)[1].upper() if a in AXES)
            self.send("ok")
        elif up.startswith("$SIM/PROBE="):
            m.probe = line.split("=", 1)[1].strip() == "1"
            self.send("ok")
        elif up.startswith("$ESPNOW/"):
            if espnow_server:
                espnow_server.command(self, up[len("$ESPNOW/"):])
            else:
                self.send("error:3")
        elif up[0] in "GMXYZF" and (m.direct_running or (not m.busy() and m.state in ("Idle", "Jog"))):
            # A G-code line sent directly is played like a one-line job.
            # Lines sent while one runs queue behind it, as in FluidNC
            # (e.g. "G91" then "G38.2 ..." from a probe button).
            queue_direct_line(m, line)
            self.send("ok")
        else:
            self.send("ok")


sessions = set()  # connected websocket sessions, for console_input() and job output
writers = set()   # their connections, for $sim/drop


def console_input(loop):
    """Reads this server's console; each line goes to every connected session."""
    def dispatch(line):
        for session in list(sessions):
            # A single character such as ~ or ! is a realtime command
            if not (len(line) == 1 and session.handle_realtime(ord(line))):
                session.handle_line(line)

    for line in sys.stdin:
        line = line.strip()
        if line:
            loop.call_soon_threadsafe(dispatch, line)


async def run_websocket(reader, writer, session):
    session.send("[MSG:INFO: FluidNC fake server for FluidTouch simulator]")
    reporter = asyncio.create_task(session.auto_report())
    sessions.add(session)
    try:
        while True:
            await writer.drain()
            opcode, data = await read_frame(reader)
            if opcode == 0x8:  # close
                writer.write(make_frame(b"", 0x8))
                break
            if opcode == 0x9:  # ping
                writer.write(make_frame(data, 0xA))
                continue
            if opcode in (0x1, 0x2):
                session.handle_input(data)
    finally:
        sessions.discard(session)
        reporter.cancel()


# ---------------------------------------------------------------------------
# ESP-NOW (FluidNC v4.0.4+ pendant protocol) over UDP
#
# The simulator's radio (sim/src/espnow_radio_udp.cpp) sends each ESP-NOW frame
# as a UDP datagram: channel, source MAC, destination MAC, encrypted flag,
# payload. This plays FluidNC's side: $espnow/pair pairing, keepalives with
# anti-replay tags, fragmented line data, realtime bytes and 200 ms status
# auto-reports. Frames sent with the wrong encryption state are rejected, as
# real hardware would.
# ---------------------------------------------------------------------------

PKT_DISCOVERY, PKT_CHALLENGE, PKT_DATA, PKT_CONFIRM = 0x01, 0x02, 0x03, 0x04
PKT_REALTIME, PKT_KEEPALIVE, PKT_RESULT, PKT_COMPLETE = 0x05, 0x06, 0x07, 0x08
PAIR_PACKETS = {PKT_DISCOVERY: "discovery", PKT_CONFIRM: "confirm", PKT_COMPLETE: "complete"}
BROADCAST = b"\xff" * 6
WINDOW_LABEL = b"fluidnc-espnow-pairing-window-v1"
SESSION_LABEL = b"fluidnc-espnow-pairing-session-v1"
MAX_PAIRINGS = 8
CHUNK = 238  # 250-byte frame minus the 12-byte data header

# NIST P-256, pure Python (fast enough for one key exchange per pairing)
P256_P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
P256_A = P256_P - 3
P256_B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
P256_N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
P256_G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
          0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)


def p256_on_curve(pt):
    x, y = pt
    return (y * y - x * x * x - P256_A * x - P256_B) % P256_P == 0


def p256_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    (x1, y1), (x2, y2) = p1, p2
    if x1 == x2:
        if (y1 + y2) % P256_P == 0:
            return None
        slope = (3 * x1 * x1 + P256_A) * pow(2 * y1, -1, P256_P) % P256_P
    else:
        slope = (y2 - y1) * pow(x2 - x1, -1, P256_P) % P256_P
    x3 = (slope * slope - x1 - x2) % P256_P
    return x3, (slope * (x1 - x3) - y1) % P256_P


def p256_mul(k, pt):
    result = None
    while k:
        if k & 1:
            result = p256_add(result, pt)
        pt = p256_add(pt, pt)
        k >>= 1
    return result


def p256_keypair():
    d = secrets.randbelow(P256_N - 1) + 1
    x, y = p256_mul(d, P256_G)
    return d, b"\x04" + x.to_bytes(32, "big") + y.to_bytes(32, "big")


def p256_shared(d, pub):
    if len(pub) != 65 or pub[0] != 4:
        raise ValueError("not an uncompressed P-256 point")
    pt = (int.from_bytes(pub[1:33], "big"), int.from_bytes(pub[33:], "big"))
    if not p256_on_curve(pt):
        raise ValueError("point not on P-256")
    return p256_mul(d, pt)[0].to_bytes(32, "big")


def hmac16(key, *parts):
    h = hmac.new(key, digestmod=hashlib.sha256)
    for part in parts:
        h.update(part)
    return h.digest()[:16]


def nonzero32():
    v = 0
    while not v:
        v = secrets.randbits(32)
    return v


def mac_str(mac):
    return ":".join(f"{b:02x}" for b in mac)


class ReplayWindow:
    """Accepts each counter once, within 64 of the highest seen, for the current challenge."""

    def __init__(self):
        self.nonce, self.top, self.seen = 0, 0, 0

    def accept(self, challenge, nonce, counter):
        if self.nonce != challenge:
            self.nonce, self.top, self.seen = challenge, 0, 0
        if nonce != challenge or counter == 0:
            return False
        if counter > self.top:
            shift = counter - self.top
            self.seen = 0 if shift >= 64 else (self.seen << shift) & (2**64 - 1)
            self.seen |= 1
            self.top = counter
            return True
        back = self.top - counter
        if back >= 64 or self.seen & (1 << back):
            return False
        self.seen |= 1 << back
        return True


class EspNowPeer:
    def __init__(self, server, mac, lmk):
        self.server, self.mac, self.lmk = server, mac, lmk
        # FluidNC sets a 200 ms auto-report interval on the ESP-NOW channel
        self.session = Session(self.out, server.machine, server.sd_root, report_interval=200)
        self.reporter = asyncio.get_event_loop().create_task(self.session.auto_report())
        self.reset()

    def reset(self):
        self.rx_nonce = nonzero32()  # our challenge: the pendant stamps its frames with it
        self.peer_nonce, self.peer_known = 0, False
        self.tx_counter = 0
        self.replay = ReplayWindow()
        self.connected = False
        self.last_rx = 0.0
        self.tx_seq = 0
        self.frag = None

    def stamp(self):
        self.tx_counter += 1
        return struct.pack("<II", self.peer_nonce, self.tx_counter)

    def out(self, data: bytes):
        """Controller output -> DATA frames (FluidNC drops output while no pendant is connected)."""
        if not (self.connected and self.peer_known):
            return
        chunks = [data[i:i + CHUNK] for i in range(0, len(data), CHUNK)][:8] or [b""]
        seq, self.tx_seq = self.tx_seq, (self.tx_seq + 1) & 0xFF
        for i, chunk in enumerate(chunks):
            self.server.send(self.mac, bytes([PKT_DATA]) + self.stamp() + bytes([seq, i, len(chunks)]) + chunk, True)

    def close(self):
        self.reporter.cancel()


class EspNowServer(asyncio.DatagramProtocol):
    def __init__(self, machine, sd_root, channel, store, hostname):
        self.machine, self.sd_root = machine, sd_root
        self.mac = bytes([0x02, 0x46, 0x4E, 0x43, 0x00, 0x01])  # locally administered, "FNC"
        self.channel, self.store, self.hostname = channel, store, hostname
        self.peers = {}   # MAC -> EspNowPeer
        self.addrs = {}   # MAC -> UDP address last heard from
        self.window_until = 0.0
        self.txn = None   # pairing transaction in progress
        self.transport = None

    def log(self, text):
        print(f"ESP-NOW: {text}")

    def connection_made(self, transport):
        self.transport = transport
        if self.store and os.path.exists(self.store):
            with open(self.store) as f:
                for entry in json.load(f):
                    mac = bytes.fromhex(entry["mac"])
                    self.peers[mac] = EspNowPeer(self, mac, bytes.fromhex(entry["lmk"]))
            self.log(f"loaded {len(self.peers)} saved pairing(s) from {self.store}")
        asyncio.get_event_loop().create_task(self.tick())

    def save(self):
        if self.store:
            os.makedirs(os.path.dirname(os.path.abspath(self.store)), exist_ok=True)
            with open(self.store, "w") as f:
                json.dump([{"mac": p.mac.hex(), "lmk": p.lmk.hex()} for p in self.peers.values()], f, indent=2)

    def send(self, mac, payload, encrypted):
        addr = self.addrs.get(mac)
        if addr and self.transport:
            header = bytes([self.channel]) + self.mac + mac + bytes([1 if encrypted else 0])
            self.transport.sendto(header + payload, addr)

    # -- $espnow/... commands -------------------------------------------------

    def command(self, session, cmd):
        if cmd == "PAIR":
            self.open_window()
            session.send("[MSG:INFO: ESP-NOW: pairing enabled for 60 seconds]")
        elif cmd == "CANCEL":
            self.window_until, self.txn = 0.0, None
            session.send("[MSG:INFO: ESP-NOW: pairing cancelled]")
        elif cmd == "LIST" or cmd == "UNPAIR":
            if not self.peers:
                session.send("[MSG:INFO: ESP-NOW: whitelist is empty]")
            for i, mac in enumerate(self.peers, 1):
                session.send(f"[MSG:INFO: ESP-NOW: {i}: {mac_str(mac)}]")
        elif cmd.startswith("UNPAIR="):
            index = int(cmd.split("=", 1)[1] or 0)
            macs = list(self.peers)
            if index == 0:
                for peer in self.peers.values():
                    peer.close()
                self.peers.clear()
                session.send("[MSG:INFO: ESP-NOW: whitelist cleared]")
            elif 1 <= index <= len(macs):
                self.peers.pop(macs[index - 1]).close()
                session.send(f"[MSG:INFO: ESP-NOW: removed peripheral {mac_str(macs[index - 1])}]")
            else:
                session.send("error:3")
                return
            self.save()
        else:
            session.send("error:3")
            return
        session.send("ok")

    def open_window(self, seconds=60):
        self.window_until = time.monotonic() + seconds
        self.txn = None
        self.log(f"pairing window open for {seconds} s on channel {self.channel}")

    # -- Frames ---------------------------------------------------------------

    def datagram_received(self, data, addr):
        if len(data) < 15:
            return
        channel, src, dst, encrypted, pkt = data[0], data[1:7], data[7:13], data[13], data[14:]
        if dst not in (self.mac, BROADCAST):
            return
        self.addrs[src] = addr
        if channel != self.channel:
            return  # our radio is on another channel
        kind = pkt[0]
        if kind in PAIR_PACKETS:
            if encrypted:
                self.log(f"rejected encrypted {PAIR_PACKETS[kind]} packet from {mac_str(src)}")
            elif kind == PKT_DISCOVERY:
                self.on_discovery(src, pkt)
            elif kind == PKT_CONFIRM:
                self.on_confirm(src, pkt)
            else:
                self.on_complete(src, pkt)
            return
        peer = self.peers.get(src)
        if not peer:
            return
        if not encrypted:
            self.log(f"rejected unencrypted packet type {kind} from paired {mac_str(src)}")
            return
        if kind == PKT_KEEPALIVE:
            self.on_keepalive(peer, pkt)
        elif kind == PKT_DATA:
            self.on_data(peer, pkt)
        elif kind == PKT_REALTIME:
            self.on_realtime(peer, pkt)

    def authenticated(self, peer):
        peer.last_rx = time.monotonic()
        if not peer.connected:
            peer.connected = True
            self.log(f"peripheral connected {mac_str(peer.mac)}")

    def on_keepalive(self, peer, pkt):
        if len(pkt) == 14:
            advertised, nonce, counter = struct.unpack_from("<III", pkt, 1)
            if not advertised or not peer.replay.accept(peer.rx_nonce, nonce, counter):
                return
            peer.peer_nonce, peer.peer_known = advertised, True
            self.authenticated(peer)
        elif len(pkt) == 5:
            (advertised,) = struct.unpack_from("<I", pkt, 1)
            if not advertised:
                return
            if not peer.peer_known or peer.peer_nonce != advertised:
                # Pendant started a new session: fresh challenge, not connected until it authenticates
                if peer.connected:
                    self.log(f"peripheral {mac_str(peer.mac)} restarted its session")
                peer.tx_counter, peer.rx_nonce, peer.replay = 0, nonzero32(), ReplayWindow()
                peer.connected = False
            peer.peer_nonce, peer.peer_known = advertised, True
        else:
            return
        reply = bytes([PKT_KEEPALIVE]) + struct.pack("<I", peer.rx_nonce)
        if peer.peer_known:
            reply += peer.stamp() + bytes([1 if peer.connected else 0])
        self.send(peer.mac, reply, True)

    def on_data(self, peer, pkt):
        if len(pkt) < 12:
            return
        nonce, counter = struct.unpack_from("<II", pkt, 1)
        if not peer.replay.accept(peer.rx_nonce, nonce, counter):
            return
        seq, index, total, payload = pkt[9], pkt[10], pkt[11], pkt[12:]
        if not 1 <= total <= 8 or index >= total:
            return
        self.authenticated(peer)
        now = time.monotonic()
        if peer.frag and (peer.frag[0] != seq or peer.frag[1] != total or now - peer.frag[3] > 3):
            peer.frag = None
        if not peer.frag:
            peer.frag = (seq, total, {}, now)
        parts = peer.frag[2]
        parts[index] = payload
        if len(parts) == total:
            message = b"".join(parts[i] for i in range(total))
            peer.frag = None
            peer.session.handle_input(message if message.endswith(b"\n") else message + b"\n")

    def on_realtime(self, peer, pkt):
        if len(pkt) != 10:
            return
        nonce, counter = struct.unpack_from("<II", pkt, 1)
        if peer.replay.accept(peer.rx_nonce, nonce, counter):
            self.authenticated(peer)
            peer.session.handle_realtime(pkt[9])

    # -- Pairing --------------------------------------------------------------

    def hostname32(self):
        return self.hostname.encode()[:31].ljust(32, b"\0")

    def on_discovery(self, src, pkt):
        if time.monotonic() > self.window_until or len(pkt) != 91:
            return
        version, mode, mac, dial_channel = pkt[1], pkt[2], pkt[3:9], pkt[9]
        session, pubkey = pkt[10:26], pkt[26:91]
        if version != 4 or mode != 1 or mac != src or session == bytes(16) or not 1 <= dial_channel <= 14:
            return
        if self.txn:
            if self.txn["mac"] == mac and self.txn["session"] == session and self.txn["state"] == "confirm":
                self.txn["last"] = time.monotonic()
                self.send(mac, self.txn["challenge"], False)
            return
        existing = self.peers.get(mac)
        if existing and existing.connected:
            self.log(f"rejecting re-pair attempt from active peripheral {mac_str(mac)}")
            return
        if not existing and len(self.peers) >= MAX_PAIRINGS:
            self.log(f"pairing roster full, rejecting {mac_str(mac)}")
            return
        d, our_pub = p256_keypair()
        try:
            shared = p256_shared(d, pubkey)
        except ValueError as e:
            self.log(f"bad public key from {mac_str(mac)}: {e}")
            return
        challenge = (bytes([PKT_CHALLENGE, 4, 1]) + self.mac + bytes([self.channel, dial_channel]) +
                     session + our_pub + self.hostname32())
        window_key = hashlib.sha256(WINDOW_LABEL).digest()[:16]
        lmk = hmac16(window_key, SESSION_LABEL, shared, pkt, challenge)
        now = time.monotonic()
        self.txn = {"mac": mac, "session": session, "lmk": lmk, "challenge": challenge,
                    "state": "confirm", "started": now, "last": now}
        self.send(mac, challenge, False)
        self.log(f"challenge sent to {mac_str(mac)}")

    def on_confirm(self, src, pkt):
        txn = self.txn
        if not txn or txn["state"] != "confirm" or len(pkt) != 41 or src != txn["mac"] or pkt[3:9] != src:
            return
        if pkt[9:25] != txn["session"] or not hmac.compare_digest(hmac16(txn["lmk"], pkt[:25]), pkt[25:41]):
            self.log(f"pairing confirmation failed from {mac_str(src)}")
            return
        result = bytes([PKT_RESULT, 4, 1]) + self.mac + bytes([self.channel]) + txn["session"] + self.hostname32()
        txn["result"] = result + hmac16(txn["lmk"], result)
        txn["state"] = "complete"
        txn["last"] = time.monotonic()
        self.send(src, txn["result"], False)

    def on_complete(self, src, pkt):
        txn = self.txn
        if not txn or txn["state"] != "complete" or len(pkt) != 41 or src != txn["mac"] or pkt[3:9] != src:
            return
        if pkt[9:25] != txn["session"] or not hmac.compare_digest(hmac16(txn["lmk"], pkt[:25]), pkt[25:41]):
            return
        old = self.peers.pop(src, None)
        if old:
            old.close()
        self.peers[src] = EspNowPeer(self, src, txn["lmk"])
        self.txn, self.window_until = None, 0.0
        self.save()
        self.log(f"paired peripheral {mac_str(src)}")

    async def tick(self):
        while True:
            await asyncio.sleep(0.05)
            now = time.monotonic()
            txn = self.txn
            if txn:
                if now > self.window_until or now - txn["last"] > 10 or now - txn["started"] > 10:
                    self.log("pairing handshake timed out")
                    self.txn = None
                elif txn["state"] == "complete" and now - txn.get("resent", 0) >= 0.3:
                    txn["resent"] = now
                    self.send(txn["mac"], txn["result"], False)
            elif self.window_until and now > self.window_until:
                self.window_until = 0.0
                self.log("pairing window closed")
            for peer in self.peers.values():
                if peer.connected and now - peer.last_rx > 10:
                    self.log(f"peripheral disconnected {mac_str(peer.mac)}")
                    peer.reset()


espnow_server = None


# ---------------------------------------------------------------------------
# HTTP (FluidNC's web server, as far as a pendant uses it)
#
# Served on the same port as the websocket, as FluidNC 4 does:
#   GET  /sd/<path>            download a file from the SD card (any other path
#                              is looked up on the flash filesystem)
#   GET  /upload?path=<dir>    list an SD folder as JSON; with
#        &action=createdir|delete|deletedir|rename&filename=<name>[&newname=]
#        do that first
#   POST /upload               multipart upload to the SD card; fields "path",
#                              "<name>S" (size) and the file part "myfiles"
#   /files                     the same for the flash filesystem
# Modelled on FluidNC's WebUIServer.cpp.
# ---------------------------------------------------------------------------

HTTP_REASONS = {200: "OK", 404: "Not Found", 500: "Internal Server Error", 503: "Service Unavailable"}
HTTP_CHUNK = 16 * 1024
MAX_UPLOAD = 64 * 1024 * 1024


def safe_join(root, rel):
    """-> absolute path of rel inside root, or None if it would escape root."""
    root = os.path.realpath(root)
    full = os.path.realpath(os.path.join(root, rel.replace("\\", "/").lstrip("/")))
    if full != root and not full.startswith(root + os.sep):
        return None
    return full


def format_bytes(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.2f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0


def file_list_json(root, path, status, list_files=True):
    folder = safe_join(root, path)
    reply = {}
    if list_files and folder and os.path.isdir(folder):
        reply["files"] = [
            {"name": name, "shortname": name, "datetime": "",
             "size": os.path.getsize(os.path.join(folder, name)) if os.path.isfile(os.path.join(folder, name)) else -1}
            for name in sorted(os.listdir(folder))]
    used = sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(root) for f in fs)
    total = 8 * 1024 ** 3  # pretend 8 GB card
    reply.update({"path": path, "total": format_bytes(total), "used": format_bytes(used),
                  "occupation": used * 100 // total, "status": status})
    return json.dumps(reply, separators=(",", ":")).encode()


def parse_multipart(body, boundary):
    """-> (fields {name: text}, file (filename, bytes) or None)"""
    fields, upload = {}, None
    for part in body.split(b"--" + boundary):
        head, sep, content = part.partition(b"\r\n\r\n")
        if not sep:
            continue
        if content.endswith(b"\r\n"):
            content = content[:-2]
        disposition = head.decode(errors="replace")
        name = re.search(r'name="([^"]*)"', disposition)
        filename = re.search(r'filename="([^"]*)"', disposition)
        if filename:
            upload = (filename.group(1), content)
        elif name:
            fields[name.group(1)] = content.decode(errors="replace")
    return fields, upload


async def http_send(writer, code, content_type, body, extra=None, rate=0):
    head = [f"HTTP/1.1 {code} {HTTP_REASONS.get(code, 'OK')}", f"Content-Type: {content_type}",
            f"Content-Length: {len(body)}", "Connection: close"] + (extra or [])
    writer.write(("\r\n".join(head) + "\r\n\r\n").encode())
    for i in range(0, len(body), HTTP_CHUNK):
        writer.write(body[i:i + HTTP_CHUNK])
        await writer.drain()
        if rate:
            await asyncio.sleep(HTTP_CHUNK / (rate * 1024.0))
    await writer.drain()


async def http_file_ops(writer, machine, root, method, query, headers, body):
    """/upload and /files: optional upload, optional action, then the listing."""
    status = "Ok"
    path = query.get("path", "")

    if method == "POST":
        boundary = re.search(r"boundary=([^;]+)", headers.get("content-type", ""))
        fields, upload = parse_multipart(body, boundary.group(1).strip().encode()) if boundary else ({}, None)
        path = fields.get("path", path)
        if upload:
            filename, content = upload
            # A filename that starts with / is the full path; otherwise it is relative to "path"
            full_name = filename if filename.startswith("/") else path.rstrip("/") + "/" + filename
            target = safe_join(root, full_name)
            expected = fields.get(full_name + "S") or fields.get(filename + "S")
            if target is None or not os.path.isdir(os.path.dirname(target)):
                await http_send(writer, 500, "text/xml", b"File creation failed")
                return
            if expected is not None and int(expected) != len(content):
                # As FluidNC: a file whose size doesn't match is discarded
                await http_send(writer, 500, "text/xml", b"Upload failed, file size mismatch")
                return
            with open(target, "wb") as f:
                f.write(content)
            print(f"  HTTP upload {full_name} ({len(content)} bytes)")
            status = "Ok"

    path = "/" + path.replace("//", "/").strip("/")
    action, filename = query.get("action"), query.get("filename")
    if action and filename:
        target = safe_join(root, path + "/" + filename)
        try:
            if target is None:
                raise OSError("bad path")
            if action == "delete":
                os.remove(target)
                status = filename + " deleted"
            elif action == "deletedir":
                shutil.rmtree(target)
                status = filename + " deleted"
            elif action == "createdir":
                os.makedirs(target, exist_ok=True)
                status = filename + " created"
            elif action == "rename":
                os.rename(target, safe_join(root, path + "/" + query.get("newname", "")))
                status = filename + " renamed to " + query.get("newname", "")
        except OSError as e:
            verb = {"createdir": "create", "rename": "rename"}.get(action, "delete")
            status = f"Cannot {verb} {filename} {e.strerror or e}"
        print(f"  HTTP {action} {path}/{filename}: {status}")

    listing = file_list_json(root, path, status, query.get("dontlist") != "yes")
    await http_send(writer, 200, "application/json", listing,
                    ["Cache-Control: no-cache"])


async def handle_http(reader, writer, machine, sd_root, localfs_root, request):
    head = request.decode(errors="replace").split("\r\n")
    try:
        method, target, _ = head[0].split(" ", 2)
    except ValueError:
        return
    headers = {k.strip().lower(): v.strip() for k, v in (line.split(":", 1) for line in head[1:] if ":" in line)}
    url = urllib.parse.urlsplit(target)
    path = urllib.parse.unquote(url.path)
    query = {k: v[0] for k, v in urllib.parse.parse_qs(url.query, keep_blank_values=True).items()}

    body = b""
    length = int(headers.get("content-length", 0) or 0)
    if length > MAX_UPLOAD:
        await http_send(writer, 500, "text/xml", b"Upload rejected, not enough space")
        return
    while len(body) < length:
        chunk = await reader.read(min(HTTP_CHUNK, length - len(body)))
        if not chunk:
            return  # the client gave up (a cancelled upload)
        body += chunk
        if machine.http_rate:
            await asyncio.sleep(len(chunk) / (machine.http_rate * 1024.0))

    if path == "/upload":
        await http_file_ops(writer, machine, sd_root, method, query, headers, body)
    elif path == "/files":
        await http_file_ops(writer, machine, localfs_root, method, query, headers, body)
    else:
        # Anything else is a file download. FluidNC can refuse these while the
        # machine moves (its HTTP/BlockDuringMotion setting).
        if machine.http_block and machine.state.split(":")[0] in ("Run", "Jog", "Home"):
            await http_send(writer, 503, "text/html", b"<html><body><h3>Cannot load WebUI while GCode Program is "
                                                      b"Running</h3></body></html>")
            return
        full = safe_join(sd_root, path[len("/sd/"):]) if path.startswith("/sd/") else safe_join(localfs_root, path)
        if full is None or not os.path.isfile(full):
            await http_send(writer, 404, "text/html", b"<html><body>404 Not Found</body></html>")
            return
        with open(full, "rb") as f:
            data = f.read()
        print(f"  HTTP download {path} ({len(data)} bytes)")
        await http_send(writer, 200, "application/octet-stream", data,
                        ["Content-Disposition: attachment"], rate=machine.http_rate)


async def handle_client(reader, writer, machine, sd_root, localfs_root):
    peer = writer.get_extra_info("peername")
    if time.monotonic() < machine.drop_until:  # $sim/drop
        writer.close()
        return
    try:
        request = await reader.readuntil(b"\r\n\r\n")
    except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
        writer.close()
        return
    key = re.search(rb"Sec-WebSocket-Key:\s*(\S+)", request, re.I)
    if not key:
        # Not a websocket handshake: a plain HTTP request
        try:
            await handle_http(reader, writer, machine, sd_root, localfs_root, request)
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            writer.close()
        return
    accept = base64.b64encode(hashlib.sha1(key.group(1) + WS_GUID.encode()).digest()).decode()
    writer.write(("HTTP/1.1 101 Switching Protocols\r\n"
                  "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Accept: {accept}\r\n\r\n").encode())
    await writer.drain()
    print(f"Client connected: {peer}")
    # FluidNC sends controller output as newline-terminated lines in binary
    # frames (a real controller may also pack several lines into one frame)
    session = Session(lambda data: writer.is_closing() or writer.write(make_frame(data)), machine, sd_root)
    writers.add(writer)
    try:
        await run_websocket(reader, writer, session)
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        writers.discard(writer)
        print(f"Client disconnected: {peer}")
        writer.close()


async def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=81)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--sd", help="folder to serve as the controller's SD card (default: sample files)")
    parser.add_argument("--espnow-port", type=int, default=8182, help="UDP port for simulated ESP-NOW (0 = off)")
    parser.add_argument("--espnow-channel", type=int, default=6, help="WiFi channel FluidNC is on (1-13)")
    parser.add_argument("--espnow-pair", action="store_true", help="open the 60 s pairing window at startup")
    parser.add_argument("--espnow-store", default=os.path.join(os.path.dirname(__file__), "..", "data",
                                                               "fake_espnow_pairings.json"),
                        help="file that remembers paired pendants ('' = don't save)")
    parser.add_argument("--hostname", default="fakefluidnc")
    parser.add_argument("--speed", type=float, default=1.0, help="play jobs this many times faster")
    parser.add_argument("--http-port", type=int, default=0,
                        help="serve HTTP on this port too (the classic UI uploads to port 80)")
    args = parser.parse_args()
    machine = Machine()
    machine.speed = max(0.01, args.speed)
    machine.broadcast = lambda line: [session.send(line) for session in list(sessions)]

    # Folders behind the SD card and the flash filesystem. Without --sd a
    # temporary card holds the sample job under a few names.
    scratch = tempfile.TemporaryDirectory(prefix="fake_fluidnc_")
    if not args.sd:
        args.sd = os.path.join(scratch.name, "sd")
        os.makedirs(os.path.join(args.sd, "projects"))
        for name in ("facing.nc", "sign_v2.gcode", "projects/bracket.nc", "projects/drawer_front.nc"):
            with open(os.path.join(args.sd, name), "wb") as f:
                f.write(SAMPLE_JOB)
        os.makedirs(os.path.join(args.sd, "fluidtouch", "macros"))
        for path, data in SAMPLE_MACROS.items():
            with open(os.path.join(args.sd, path[len("/sd/"):]), "wb") as f:
                f.write(data)
    localfs = os.path.join(scratch.name, "localfs")
    os.makedirs(localfs)

    def on_client(r, w):
        return handle_client(r, w, machine, args.sd, localfs)

    server = await asyncio.start_server(on_client, args.host, args.port)
    print(f"Fake FluidNC listening on ws://{args.host}:{args.port}/ and http://{args.host}:{args.port}/ "
          f"(Ctrl+C to stop)")
    print(f"SD card folder: {args.sd}")
    if args.http_port and args.http_port != args.port:
        await asyncio.start_server(on_client, args.host, args.http_port)
        print(f"HTTP also on port {args.http_port}")
    threading.Thread(target=console_input, args=(asyncio.get_running_loop(),), daemon=True).start()
    if args.espnow_port:
        global espnow_server
        espnow_server = EspNowServer(machine, args.sd, args.espnow_channel, args.espnow_store, args.hostname)
        await asyncio.get_running_loop().create_datagram_endpoint(
            lambda: espnow_server, local_addr=(args.host, args.espnow_port))
        print(f"ESP-NOW on UDP {args.host}:{args.espnow_port}, channel {args.espnow_channel}, "
              f"MAC {mac_str(espnow_server.mac)}")
        if args.espnow_pair:
            espnow_server.open_window()
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
