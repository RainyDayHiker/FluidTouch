#!/bin/bash
# Starts the simulator without a window in both UI modes, checks that each UI
# comes up, and leaves a screenshot of each in <out>/. Used by CI
# (.github/workflows/build-lrt.yml); run from the repository root in a shell
# that has the simulator's DLLs on its PATH (MSYS2 UCRT64, or Linux).
#
#   sim/tools/smoke_test.sh [out-dir]
set -u

out="${1:-sim/build/smoke}"
exe="$(pwd)/sim/build/fluidtouch_sim"
[ -x "$exe" ] || exe="$exe.exe"
rm -rf "$out"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

# No window: SDL's dummy video driver with the software renderer
export SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software

failed=0

# run <name> <text the log must contain>; <out>/<name>/ is the data folder
run() {
    local name="$1" expect="$2" data="$out/$1"
    (cd "$(dirname "$exe")" && timeout 60 "$exe" --data "$data" --script "wait 4000; shot $name.bmp; exit") \
        > "$out/$name.log" 2>&1 < /dev/null
    local code=$?
    if [ $code -ne 0 ]; then
        echo "FAIL $name: simulator exited with $code"; failed=1
    elif ! grep -q "$expect" "$out/$name.log"; then
        echo "FAIL $name: log does not contain '$expect'"; failed=1
    elif [ ! -s "$data/screenshots/$name.bmp" ]; then
        echo "FAIL $name: no screenshot"; failed=1
    else
        cp "$data/screenshots/$name.bmp" "$out/$name.bmp"
        echo "ok   $name"
    fi
    [ $failed -eq 0 ] || tail -n 30 "$out/$name.log"
}

# New UI: the default on a fresh device
mkdir -p "$out/new-ui"
run new-ui "LrtApp: starting LowRider Touch"

# Classic UI: selected by the UI mode key (specs/02-architecture.md section 3)
mkdir -p "$out/classic-ui"
echo '{"lrt":{"ui_mode":{"type":"u8","value":1}}}' > "$out/classic-ui/preferences.json"
run classic-ui "Machine selection screen displayed"

exit $failed
