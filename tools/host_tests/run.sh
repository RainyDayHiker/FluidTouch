#!/bin/bash
# Builds and runs the host tests of src/services code that has no Arduino calls (MSYS2 g++, as
# the simulator uses). Usage, from the repo root:
#
#   tools/host_tests/run.sh <G-code files or folders>...
#
# A folder is searched for files named "input" (the device tests' cases) and *.gcode / *.nc.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/lrt_host_tests"
mkdir -p "$OUT"
export PATH="/c/msys64/ucrt64/bin:$PATH"
g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -I"$ROOT/lib/gcode/include" -I"$ROOT/src" \
    "$ROOT/tools/host_tests/segment_list_test.cpp" "$ROOT/src/services/segment_list.cpp" \
    "$ROOT/src/services/run_tracker.cpp" \
    "$ROOT"/lib/gcode/src/*.cpp -o "$OUT/segment_list_test.exe"
FILES=()
for a in "$@"; do
    if [ -d "$a" ]; then
        while IFS= read -r -d '' f; do FILES+=("$f"); done < <(find "$a" -type f \( -name input -o -name '*.gcode' -o -name '*.nc' \) -print0 | sort -z)
    else
        FILES+=("$a")
    fi
done
"$OUT/segment_list_test.exe" "${FILES[@]}"
