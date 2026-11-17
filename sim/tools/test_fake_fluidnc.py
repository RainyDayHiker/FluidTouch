"""Checks the fake FluidNC's job playback in-process (no sockets) and prints a
timeline: the sample job with its prompts, probe and messages, then feed hold,
reset while moving, a probe that finds nothing, and feed hold when idle. Then
the HTTP side over a real socket: download, listing, upload, file actions.

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


# --- HTTP: download, listing, upload, actions --------------------------------

def http(port, method, target, body=None, content_type=None):
    """-> (status code, body bytes); blocking, run in a thread"""
    import http.client
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    headers = {"Content-Type": content_type} if content_type else {}
    conn.request(method, target, body=body, headers=headers)
    response = conn.getresponse()
    data = response.read()
    conn.close()
    return response.status, data


def multipart(path, name, content, size=None):
    boundary = "----testboundary"
    size = len(content) if size is None else size
    parts = [f'--{boundary}\r\nContent-Disposition: form-data; name="path"\r\n\r\n{path}\r\n'.encode(),
             f'--{boundary}\r\nContent-Disposition: form-data; name="{name}S"\r\n\r\n{size}\r\n'.encode(),
             f'--{boundary}\r\nContent-Disposition: form-data; name="myfiles"; filename="{name}"\r\n'
             f'Content-Type: application/octet-stream\r\n\r\n'.encode() + content,
             f'\r\n--{boundary}--\r\n'.encode()]
    return b"".join(parts), f"multipart/form-data; boundary={boundary}"


async def http_checks():
    import json, tempfile
    m = fake.Machine()
    with tempfile.TemporaryDirectory() as tmp:
        sd, localfs = os.path.join(tmp, "sd"), os.path.join(tmp, "localfs")
        os.makedirs(sd); os.makedirs(localfs)
        with open(os.path.join(sd, "job.nc"), "wb") as f:
            f.write(fake.SAMPLE_JOB)
        server = await asyncio.start_server(lambda r, w: fake.handle_client(r, w, m, sd, localfs), "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        loop = asyncio.get_running_loop()

        async def call(*args):
            return await loop.run_in_executor(None, http, port, *args)

        code, data = await call("GET", "/sd/job.nc")
        assert code == 200 and data == fake.SAMPLE_JOB
        code, data = await call("GET", "/sd/missing.nc")
        assert code == 404
        code, data = await call("GET", "/sd/../../etc/passwd")
        assert code == 404

        code, data = await call("GET", "/upload?path=/")
        listing = json.loads(data)
        assert code == 200 and listing["files"] == [
            {"name": "job.nc", "shortname": "job.nc", "datetime": "", "size": len(fake.SAMPLE_JOB)}], listing

        code, data = await call("GET", "/upload?action=createdir&filename=lrt&path=/&dontlist=yes")
        assert code == 200 and json.loads(data)["status"] == "lrt created" and "files" not in json.loads(data)

        content = os.urandom(300000)
        body, ctype = multipart("/lrt/", "/lrt/new.nc", content)
        code, data = await call("POST", "/upload", body, ctype)
        assert code == 200 and json.loads(data)["files"][0]["size"] == len(content), data
        code, data = await call("GET", "/sd/lrt/new.nc")
        assert code == 200 and data == content

        body, ctype = multipart("/lrt/", "/lrt/short.nc", content, size=len(content) + 1)
        code, data = await call("POST", "/upload", body, ctype)
        assert code == 500 and not os.path.exists(os.path.join(sd, "lrt", "short.nc"))
        body, ctype = multipart("/nodir/", "/nodir/x.nc", content)
        code, data = await call("POST", "/upload", body, ctype)
        assert code == 500

        code, data = await call("GET", "/upload?action=delete&filename=new.nc&path=/lrt")
        assert json.loads(data)["status"] == "new.nc deleted" and json.loads(data)["files"] == []

        # the flash filesystem is separate
        body, ctype = multipart("/", "/config.yaml", b"name: fake\n")
        code, data = await call("POST", "/files", body, ctype)
        assert code == 200
        code, data = await call("GET", "/config.yaml")
        assert code == 200 and data == b"name: fake\n"

        # downloads can be refused while moving
        m.http_block, m.state = True, "Run"
        code, data = await call("GET", "/sd/job.nc")
        assert code == 503
        m.state = "Idle"
        code, data = await call("GET", "/sd/job.nc")
        assert code == 200
        server.close()
    print("HTTP CHECKS PASSED")


asyncio.run(http_checks())
