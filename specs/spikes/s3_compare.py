"""S3 offline comparison (specs/06-test-and-spikes.md): the SD: percent in a position log
(what FluidNC has read) against the line the tool is on, found from the logged position.

    python specs/spikes/s3_compare.py specs/spikes/s3_air_cut.gcode <log.csv>

The tool's line is the segment nearest each logged position, chosen so that it never goes
backwards and never passes the line read, with the least total distance over the run.
Rapids and arcs are sampled, so a row in the middle of a long rapid can look a few mm off.
"""
import csv, math, re, sys, bisect

job, logf = sys.argv[1], sys.argv[2]
raw = open(job, "rb").read()
size = len(raw)
lines = raw.split(b"\n")
if lines and lines[-1] == b"":
    lines = lines[:-1]

# byte offset just after each line (incl. newline)
ends, off = [], 0
for ln in lines:
    off += len(ln) + 1
    ends.append(off)

# motion segments: (line index, list of sample points)
segs = []
x = y = z = 0.0
mode = "G0"
for i, b in enumerate(lines):
    s = b.decode().split("(")[0].strip().upper()
    if not s:
        continue
    g = re.match(r"G0?([0123])\b", s)
    if g:
        mode = "G" + g.group(1)
    if s.startswith(("G10", "G4", "G21", "M")):
        continue
    w = dict((a, float(v)) for a, v in re.findall(r"([XYZIJ])(-?[\d.]+)", s))
    if not any(k in w for k in "XYZ"):
        continue
    nx, ny, nz = w.get("X", x), w.get("Y", y), w.get("Z", z)
    if mode in ("G2", "G3") and ("I" in w or "J" in w):
        cx, cy = x + w.get("I", 0), y + w.get("J", 0)
        r = math.hypot(x - cx, y - cy)
        a0 = math.atan2(y - cy, x - cx); a1 = math.atan2(ny - cy, nx - cx)
        cw = mode == "G2"
        sweep = ((a0 - a1) if cw else (a1 - a0)) % (2 * math.pi) or 2 * math.pi
        geo = ("A", cx, cy, r, a0, sweep, cw, z, nz)
    else:
        geo = ("L", (x, y, z), (nx, ny, nz))
    segs.append((i, geo))
    x, y, z = nx, ny, nz


def dist(p, geo):
    """Exact distance from p to a straight move, or to an XY arc (helix: Z by angle)."""
    if geo[0] == "L":
        a, b = geo[1], geo[2]
        ab = [b[k] - a[k] for k in range(3)]
        l2 = sum(v * v for v in ab)
        t = 0.0 if l2 == 0 else max(0.0, min(1.0, sum((p[k] - a[k]) * ab[k] for k in range(3)) / l2))
        return math.dist(p, [a[k] + ab[k] * t for k in range(3)])
    _, cx, cy, r, a0, sweep, cw, z0, z1 = geo
    ap = math.atan2(p[1] - cy, p[0] - cx)
    t = ((a0 - ap) if cw else (ap - a0)) % (2 * math.pi)
    if t > sweep:   # outside the sweep: the nearer end
        t = sweep if t - sweep < 2 * math.pi - t else 0.0
    a = a0 + (-t if cw else t)
    q = (cx + r * math.cos(a), cy + r * math.sin(a), z0 + (z1 - z0) * t / sweep)
    return math.dist(p, q)

rows = list(csv.DictReader(open(logf)))

def read_line(pct):
    """Line FluidNC has read up to: the line whose end is at or past pct of the file."""
    b = float(pct) / 100 * size
    return min(bisect.bisect_left(ends, b - 0.5), len(lines) - 1)

# tool line: a non-decreasing segment per row, never past the read line, that
# minimises the total distance (dynamic programming over a window of candidates)
seg_line = [sg[0] for sg in segs]
data = [r for r in rows if r["percent"]]
prev = None   # dict seg index -> (cost, back)
trace = []
for r in data:
    p = (float(r["wx"]), float(r["wy"]), float(r["wz"]))
    rl = read_line(r["percent"])
    hi = bisect.bisect_right(seg_line, rl)
    cands = range(max(0, hi - 70), hi)
    cur = {}
    if prev is None:
        for k in cands:
            cur[k] = (dist(p, segs[k][1]), None)
    else:
        keys = sorted(prev)
        # prefix minimum of prev cost over keys <= k
        pm, best, j = {}, (1e18, None), 0
        for k in cands:
            while j < len(keys) and keys[j] <= k:
                if prev[keys[j]][0] < best[0]:
                    best = (prev[keys[j]][0], keys[j])
                j += 1
            if best[1] is None:
                continue
            d = dist(p, segs[k][1])
            cur[k] = (best[0] + d, best[1])
    trace.append(cur)
    prev = cur
k = min(prev, key=lambda k: prev[k][0])
path = []
for cur in reversed(trace):
    path.append(k)
    k = cur[k][1]
path.reverse()
out = []
for r, k in zip(data, path):
    p = (float(r["wx"]), float(r["wy"]), float(r["wz"]))
    d = dist(p, segs[k][1])
    out.append((int(r["ms"]), r["state"], float(r["percent"]), read_line(r["percent"]),
                segs[k][0], d, r["message"]))

print(f"file {size} bytes, {len(lines)} lines, {len(rows)} rows")
off = [o for o in out if o[5] > 1.0]
print(f"rows more than 1 mm from their matched move: {len(off)}"
      + "".join(f"\n  {o[0]/1000:.1f}s line {o[4] + 1}: {lines[o[4]].decode()} ({o[5]:.1f} mm)"
                for o in sorted(off, key=lambda o: -o[5])[:8]))

# per-row lag in lines; per-section summary
def section(line):
    name = "start"
    for i in range(line + 1):
        m = re.search(rb"Operation (\d)", lines[i])
        if m and lines[i].startswith(b"(MSG"):
            name = "op" + m.group(1).decode()
    return name

print("\nsection   rows  lag lines (min/median/max)   percent range")
by = {}
for o in out:
    by.setdefault(section(o[4]), []).append(o)
for k, v in by.items():
    lags = sorted(o[3] - o[4] for o in v)
    print(f"{k:8} {len(v):5}   {lags[0]:4} / {lags[len(lags)//2]:4} / {lags[-1]:4}"
          f"          {v[0][2]:6.2f} .. {v[-1][2]:6.2f}")

# time lag: when each line was first read vs when the tool first reached it
first_read, first_tool = {}, {}
for o in out:
    for L in range(0, o[3] + 1):
        first_read.setdefault(L, o[0])
    first_tool.setdefault(o[4], o[0])
print("\nseconds between reading a line and the tool reaching it")
for k in sorted(set(section(L) for L in first_tool)):
    d = [(first_tool[L] - first_read[L]) / 1000 for L in first_tool
         if L in first_read and section(L) == k]
    d.sort()
    if d:
        print(f"{k:8} n={len(d):4}  min {d[0]:5.1f}  median {d[len(d)//2]:5.1f}  max {d[-1]:5.1f}")

# messages: where the tool was when the message line was read
print("\nmessages")
for o in out:
    if o[6]:
        mline = next((i for i, b in enumerate(lines) if o[6].encode() in b), None)
        print(f"{o[0]/1000:7.1f}s  {o[6][:45]:45} msg line {mline}  tool line {o[4]}"
              f" ({section(o[4])})  pct {o[2]:.2f}")

# percent vs time (time fraction of the run)
t0, t1 = out[0][0], out[-1][0]
print("\npercent vs elapsed time")
for f in (0.1, 0.25, 0.4, 0.5, 0.6, 0.75, 0.9):
    t = t0 + f * (t1 - t0)
    o = min(out, key=lambda o: abs(o[0] - t))
    print(f"{f*100:3.0f}% of time -> {o[2]:6.2f}% read")

# report gaps
gaps = [(rows[i + 1]["ms"], int(rows[i + 1]["ms"]) - int(rows[i]["ms"]))
        for i in range(len(rows) - 1)]
gs = sorted(g[1] for g in gaps)
print(f"\nreport gaps ms: median {gs[len(gs)//2]}, 95% {gs[int(len(gs)*.95)]}, max {gs[-1]}")
print("gaps over 400 ms:", [(int(a) / 1000, g) for a, g in gaps if g > 400][:20])
