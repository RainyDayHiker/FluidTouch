"""Checks the fake FluidNC's job playback in-process (no sockets) and prints a
timeline: the sample job with its prompts, probe and messages, then feed hold,
reset while moving, a probe that finds nothing, and feed hold when idle.

    python sim/tools/test_fake_fluidnc.py
"""
import asyncio, importlib.util, os, re, sys, time
spec = importlib.util.spec_from_file_location(
    "fake", os.path.join(os.path.dirname(os.path.abspath(__file__)), "fake_fluidnc.py"))
fake = importlib.util.module_from_spec(spec); spec.loader.exec_module(fake)

async def main():
    m = fake.Machine(); m.speed = 60
    out = []
    session = fake.Session(lambda data: out.append(data.decode().strip()), m, None)
    m.broadcast = session.send
    t0 = time.monotonic(); log = []
    def note(what): log.append(f"{time.monotonic()-t0:5.2f}s {what}")
    session.handle_line("$SD/Run=/sd/sample.nc"); note("start: " + out[-1])
    seen = 0; last = None; resumes = 0; states = []; sd_seen = []; max_x = 0
    while m.busy() and time.monotonic() - t0 < 60:
        await asyncio.sleep(0.02)
        for line in out[seen:]: note("<- " + line)
        seen = len(out)
        st = m.status(); state = st[1:st.index("|")]
        sd = re.search(r"SD:([\d.]+),", st); sd_seen.append(float(sd.group(1)) if sd else None)
        max_x = max(max_x, m.mpos[0])
        if state != last:
            note(f"state {state}  {st}"); last = state; states.append(state)
            if state == "Hold:0":
                await asyncio.sleep(0.1); session.handle_realtime(ord("~")); resumes += 1; note("-> ~")
    for line in out[seen:]: note("<- " + line)
    note("end: " + m.status())
    print("\n".join(log))
    pcts = [p for p in sd_seen if p is not None]
    assert resumes == 3, resumes
    assert pcts == sorted(pcts) and pcts[0] < 30 and pcts[-1] > 80, (pcts[0], pcts[-1])
    assert sd_seen[-1] is None
    assert any("PRB:" in l and l.endswith(":1]") for l in out)
    assert sum("MSG,Operation" in l for l in out) == 2
    assert abs(max_x - 290) < 0.01, max_x; print("percent steps:", len(set(pcts)), "first", pcts[0], "last", pcts[-1])
    assert m.state == "Idle" and abs(m.mpos[0]) < 1e-6 and abs(m.mpos[1]) < 1e-6

    # feed hold, resume, then reset while moving
    out.clear(); m.speed = 1
    session.handle_line("G1 X100 F600"); await asyncio.sleep(0.3)
    assert m.state == "Run", m.state
    session.handle_realtime(ord("!")); await asyncio.sleep(0.15); assert m.state == "Hold:0", m.state
    x = m.mpos[0]; await asyncio.sleep(0.2); assert m.mpos[0] == x
    session.handle_realtime(ord("~")); await asyncio.sleep(0.2); assert m.state == "Run" and m.mpos[0] > x
    session.handle_realtime(0x18); await asyncio.sleep(0.15)
    assert m.state == "Alarm" and "ALARM:3" in out and not m.busy(), (m.state, out)
    session.handle_line("$X"); assert m.state == "Idle"
    # probe miss
    out.clear(); m.speed = 50; session.handle_line("$sim/probemiss=1"); session.handle_line("G38.2 Z-10 F100")
    await asyncio.sleep(0.5)
    assert m.state == "Alarm" and "ALARM:5" in out and any(l.endswith(":0]") for l in out), out
    # idle feed hold is ignored
    session.handle_line("$X"); session.handle_realtime(ord("!")); assert m.state == "Idle"
    print("ALL CHECKS PASSED")
asyncio.run(main())
