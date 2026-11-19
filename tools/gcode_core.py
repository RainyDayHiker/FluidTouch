#!/usr/bin/env python3
"""The G-code library from a CNC-Tools release (specs/04 §1, D69).

    python tools/gcode_core.py fetch <version> [--zip <archive>]
        Downloads gcode-core-<version>.zip from the CNC-Tools release tagged v<version> (needs
        the GitHub CLI, signed in with access to CNC-Tools), checks it against its SHA256SUMS,
        and copies:
          library/                 -> lib/gcode/
          device-tests/runner/     -> src/services/gcode_device/   (runner.h, runner.cpp)
        each with the archive's manifest.json and a SHA256SUMS for exactly those files.
        --zip uses an archive already downloaded.

    python tools/gcode_core.py check
        Checks both copies against their SHA256SUMS: every file listed matches, and nothing
        else is there. CI runs it; a copy is never edited (a change goes into CNC-Tools).

    python tools/gcode_core.py data <version> <folder> [--zip <archive>]
        Unpacks the device suite's data (device-tests/data/) into <folder>, checked, for the
        display SD card's /gcode-tests/ folder. Never into this repo.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CNC_TOOLS = "RainyDayHiker/CNC-Tools"
COPIES = {
    # archive folder -> (destination in this repo, files left out)
    "library/": ("lib/gcode", set()),
    # host_main.cpp has a main() for CNC-Tools' CI; the display has its own platform code
    "device-tests/runner/": ("src/services/gcode_device", {"host_main.cpp"}),
}
OWN = {"SHA256SUMS", "manifest.json"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def read_sums(path):
    sums = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line:
                digest, name = line.split("  ", 1)
                sums[name] = digest
    return sums


def unpack(version, zip_path, work):
    """-> the unpacked archive's top folder, after checking every file against SHA256SUMS."""
    name = f"gcode-core-{version}"
    if not zip_path:
        subprocess.run(["gh", "release", "download", f"v{version}", "-R", CNC_TOOLS,
                        "-p", name + ".zip", "-D", work], check=True)
        zip_path = os.path.join(work, name + ".zip")
    with zipfile.ZipFile(zip_path) as z:
        for member in z.namelist():  # no absolute paths or .. in the archive
            if member.startswith("/") or ".." in member.split("/"):
                sys.exit(f"unsafe path in the archive: {member}")
        z.extractall(work)
    top = os.path.join(work, name)
    if not os.path.isdir(top):
        sys.exit(f"the archive has no folder {name}/")
    sums = read_sums(os.path.join(top, "SHA256SUMS"))
    for rel, digest in sums.items():
        if sha256(os.path.join(top, rel)) != digest:
            sys.exit(f"checksum mismatch in the archive: {rel}")
    manifest = json.load(open(os.path.join(top, "manifest.json"), encoding="utf-8"))
    if manifest.get("version") != version:
        sys.exit(f"the archive says version {manifest.get('version')}, not {version}")
    print(f"{name}.zip: {len(sums)} files checked; phase {manifest.get('phase')}, "
          f"source {manifest.get('source', '')[:12]}")
    return top, sums, manifest


def fetch(version, zip_path):
    with tempfile.TemporaryDirectory() as work:
        top, sums, manifest = unpack(version, zip_path, work)
        for folder, (dest, leave_out) in COPIES.items():
            target = os.path.join(REPO, dest)
            shutil.rmtree(target, ignore_errors=True)
            lines = []
            for rel, digest in sorted(sums.items()):
                if not rel.startswith(folder) or os.path.basename(rel) in leave_out:
                    continue
                inner = rel[len(folder):]
                os.makedirs(os.path.dirname(os.path.join(target, inner)), exist_ok=True)
                shutil.copyfile(os.path.join(top, rel), os.path.join(target, inner))
                lines.append(f"{digest}  {inner}")
            shutil.copyfile(os.path.join(top, "manifest.json"), os.path.join(target, "manifest.json"))
            lines.append(f"{sums['manifest.json']}  manifest.json")
            with open(os.path.join(target, "SHA256SUMS"), "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(lines) + "\n")
            print(f"{dest}: {len(lines)} files")
    print(f"Now: python tools/gcode_core.py check, build, and record {version} in specs/HANDOFF.md")


def check():
    bad = 0
    for dest, _ in COPIES.values():
        target = os.path.join(REPO, dest)
        sums_path = os.path.join(target, "SHA256SUMS")
        if not os.path.exists(sums_path):
            print(f"{dest}: no SHA256SUMS (not fetched)")
            bad += 1
            continue
        sums = read_sums(sums_path)
        present = set()
        for root, _, files in os.walk(target):
            for f in files:
                present.add(os.path.relpath(os.path.join(root, f), target).replace(os.sep, "/"))
        for rel in sorted(present - set(sums) - {"SHA256SUMS"}):
            print(f"{dest}/{rel}: not in the release (copies are never edited)")
            bad += 1
        for rel, digest in sorted(sums.items()):
            path = os.path.join(target, rel)
            if not os.path.exists(path):
                print(f"{dest}/{rel}: missing")
                bad += 1
            elif sha256(path) != digest:
                print(f"{dest}/{rel}: changed (copies are never edited; line endings? see .gitattributes)")
                bad += 1
        version = json.load(open(os.path.join(target, "manifest.json"), encoding="utf-8")).get("version")
        print(f"{dest}: {len(sums)} files, gcode-core {version}")
    if bad:
        sys.exit(f"{bad} problem(s)")
    print("copies match their releases")


def data(version, folder, zip_path):
    with tempfile.TemporaryDirectory() as work:
        top, sums, _ = unpack(version, zip_path, work)
        src = os.path.join(top, "device-tests", "data")
        os.makedirs(folder, exist_ok=True)
        shutil.copytree(src, folder, dirs_exist_ok=True)
        n = sum(1 for rel in sums if rel.startswith("device-tests/data/"))
        print(f"{folder}: {n} files of the device suite's data (checked); copy them to the "
              f"display SD as /gcode-tests/")


def main():
    args = sys.argv[1:]
    zip_path = None
    if "--zip" in args:
        i = args.index("--zip")
        zip_path = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    if args[:1] == ["fetch"] and len(args) == 2:
        fetch(args[1], zip_path)
    elif args[:1] == ["check"] and len(args) == 1:
        check()
    elif args[:1] == ["data"] and len(args) == 3:
        data(args[1], os.path.abspath(args[2]), zip_path)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
