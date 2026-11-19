#!/usr/bin/env python3
"""Test files for spikes S3 and S4 (specs/06-test-and-spikes.md).

    python specs/spikes/make_spike_jobs.py            # writes specs/spikes/s3_air_cut.gcode
    python specs/spikes/make_spike_jobs.py --s4 <dir> # also s4_5mb.gcode and s4_9_5mb.gcode in <dir>

S3 air cut: zeroes X, Y and Z where the tool is when it starts, and then stays
at Z 1 or above and inside X 0..175, Y 0..115. Five operations announced with
(MSG ...): full circles in three passes (G2), a zigzag of straight lines, a
zigzag with arc turns (G2/G3), a circle of 720 short G1 lines, and half
circles making growing circles that drift right (meant as a spiral; kept as run in S3, see
specs/06). Prompts (M0) at the start, halfway and the end; a G4
dwell between the first two operations. These are the lines FluidNC treats
differently when it reports the SD: percent (M0, G4, G10 wait for motion).

S4 files are for uploading only: a long harmless path at Z 10, starting with
an M0 that says not to run them.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SAFE_Z = 10.0


class Job:
    def __init__(self):
        self.lines = []
        self.x = self.y = 0.0
        self.z = 0.0
        self.cut = 0.0    # mm at feed
        self.time = 0.0   # minutes at the programmed feeds, rapids at 2400

    def add(self, line):
        self.lines.append(line)

    def rapid(self, x=None, y=None, z=None):
        words = []
        nx, ny, nz = self.x, self.y, self.z
        if x is not None:
            nx = x
            words.append(f"X{x:.3f}")
        if y is not None:
            ny = y
            words.append(f"Y{y:.3f}")
        if z is not None:
            nz = z
            words.append(f"Z{z:.3f}")
        self.time += math.dist((self.x, self.y, self.z), (nx, ny, nz)) / 2400
        self.x, self.y, self.z = nx, ny, nz
        self.add("G0 " + " ".join(words))

    def line(self, x=None, y=None, z=None, f=None):
        nx = self.x if x is None else x
        ny = self.y if y is None else y
        nz = self.z if z is None else z
        words = []
        if x is not None:
            words.append(f"X{x:.3f}")
        if y is not None:
            words.append(f"Y{y:.3f}")
        if z is not None:
            words.append(f"Z{z:.3f}")
        if f is not None:
            words.append(f"F{f:.0f}")
            self.feed = f
        d = math.dist((self.x, self.y, self.z), (nx, ny, nz))
        self.cut += d
        self.time += d / self.feed
        self.x, self.y, self.z = nx, ny, nz
        self.add("G1 " + " ".join(words))

    def arc(self, cw, x, y, i, j, f=None):
        """Half or full circle in XY; the length is worked out from the sweep."""
        cx, cy = self.x + i, self.y + j
        r = math.hypot(i, j)
        a0 = math.atan2(self.y - cy, self.x - cx)
        a1 = math.atan2(y - cy, x - cx)
        sweep = (a0 - a1) if cw else (a1 - a0)
        sweep %= 2 * math.pi
        if sweep < 1e-9:
            sweep = 2 * math.pi
        words = [f"X{x:.3f}", f"Y{y:.3f}", f"I{i:.3f}", f"J{j:.3f}"]
        if f is not None:
            words.append(f"F{f:.0f}")
            self.feed = f
        d = r * sweep
        self.cut += d
        self.time += d / self.feed
        self.x, self.y = x, y
        self.add(("G2 " if cw else "G3 ") + " ".join(words))


def s3_job():
    j = Job()
    j.add("(S3 air cut for LowRider Touch, made by specs/spikes/make_spike_jobs.py)")
    j.add("(Zeroes X Y Z where the tool is; then stays at Z 0 or above, X 0..175, Y 0..115)")
    j.add("G21 G90 G94 G17")
    j.add("G10 L20 P0 X0 Y0 Z0")
    j.rapid(z=SAFE_Z)
    j.add("M0 (MSG S3 air cut - router off and position log on - Continue)")

    # 1: four circles, three passes each, full-circle G2
    j.add("(MSG Operation 1 of 5: Circles)")
    for cx, cy in ((30, 30), (85, 30), (30, 85), (85, 85)):
        j.rapid(x=cx + 20, y=cy)
        for z in (6.0, 4.0, 2.0):
            j.line(z=z, f=300)
            j.arc(True, cx + 20, cy, -20, 0, f=800)
        j.rapid(z=SAFE_Z)
    j.add("G4 P1.5")

    # 2: zigzag of straight lines, X 115..175, Y 5..60
    j.add("(MSG Operation 2 of 5: Zigzag)")
    j.rapid(x=115, y=5)
    j.line(z=2.0, f=300)
    y, forward = 5.0, True
    while y <= 60.0 + 1e-9:
        j.line(x=175 if forward else 115, f=1200)
        y += 2.5
        if y <= 60.0 + 1e-9:
            j.line(y=y)
        forward = not forward
    j.rapid(z=SAFE_Z)
    j.add("M0 (MSG Halfway - Continue)")

    # 3: zigzag with half-circle turns, X 115..175, Y 65..115
    j.add("(MSG Operation 3 of 5: Rounded zigzag)")
    # The turns bulge out by r, so the straight lines end r short of the edges
    r = 2.5
    left, right = 115 + r, 175 - r
    j.rapid(x=left, y=65)
    j.line(z=3.0, f=300)
    y, forward = 65.0, True
    while y + 2 * r <= 115.0 + 1e-9:
        if forward:
            j.line(x=right, f=1000)
            j.arc(False, right, y + 2 * r, 0, r)   # turn up on the right (G3)
        else:
            j.line(x=left, f=1000)
            j.arc(True, left, y + 2 * r, 0, r)     # turn up on the left (G2)
        y += 2 * r
        forward = not forward
    j.line(x=right if forward else left)
    j.rapid(z=SAFE_Z)

    # 4: a circle as 720 short lines (0.22 mm each)
    j.add("(MSG Operation 4 of 5: Circle of short lines)")
    cx, cy, rr = 57.5, 57.5, 25.0
    j.rapid(x=cx + rr, y=cy)
    j.line(z=1.0, f=300)
    for k in range(1, 721):
        a = 2 * math.pi * k / 720
        j.line(x=cx + rr * math.cos(a), y=cy + rr * math.sin(a), f=1500 if k == 1 else None)
    j.rapid(z=SAFE_Z)

    # 5: half circles, G2 only. Meant as a spiral about (145, 32.5), but the left end stays
    # at X 144: each pair is a circle 1.5 mm wider, drifting right. Kept as run in S3.
    j.add("(MSG Operation 5 of 5: Spiral)")
    cx, cy, step = 145.0, 32.5, 1.5
    j.rapid(x=cx + 1.0, y=cy)
    j.line(z=1.5, f=300)
    radius, centre = 1.0, cx
    for _ in range(36):
        # half circle about (centre, cy), from the right side to the left
        end = centre - radius
        j.arc(True, end, cy, centre - j.x, 0, f=900)
        # the next one is centred half a step right and half a step bigger
        centre += step / 2
        radius += step / 2
        end = centre + radius
        j.arc(True, end, cy, centre - j.x, 0)
        if end + step / 2 > 175:
            break
    j.rapid(z=SAFE_Z)

    j.rapid(x=0, y=0)
    j.add("M0 (MSG S3 complete - Continue to end)")
    j.add("M2")
    return j


def s4_file(path, size):
    with open(path, "w", newline="\n") as f:
        f.write("(S4 upload test file, made by specs/spikes/make_spike_jobs.py - not a job)\n")
        f.write("M0 (MSG S4 upload test file - do not run it - Abort)\n")
        f.write("G21 G90\nG0 Z10\n")
        k = 0
        while f.tell() < size:
            x = 10 + (k % 100) * 0.5
            y = 10 + (k // 100 % 100) * 0.5
            f.write(f"G1 X{x:.3f} Y{y:.3f} Z10.000 F1000\n")
            k += 1
        f.write("M2\n")
    print(f"{path}: {os.path.getsize(path) / 1e6:.1f} MB")


def main():
    j = s3_job()
    out = os.path.join(HERE, "s3_air_cut.gcode")
    with open(out, "w", newline="\n") as f:
        f.write("\n".join(j.lines) + "\n")
    print(f"{out}: {len(j.lines)} lines, {j.cut / 1000:.1f} m at feed, "
          f"about {j.time:.1f} min at the programmed feeds (before acceleration and prompts)")
    if "--s4" in sys.argv:
        d = sys.argv[sys.argv.index("--s4") + 1]
        os.makedirs(d, exist_ok=True)
        s4_file(os.path.join(d, "s4_5mb.gcode"), 5 * 1000 * 1000)
        # Classic's UploadManager refuses files over 10 MB (10485760 bytes); D63
        s4_file(os.path.join(d, "s4_9_5mb.gcode"), 9_500_000)


if __name__ == "__main__":
    main()
