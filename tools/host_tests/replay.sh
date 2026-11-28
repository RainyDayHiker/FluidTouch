#!/bin/bash
# A job's time estimate, and Run's time left replayed against a position log, with the
# display's own code (log_replay.cpp; MSYS2 g++, as the simulator uses). From the repo root:
#
#   tools/host_tests/replay.sh <file.gcode> [position-log.csv] [x,y,z axis rates mm/min]
#
# Rates default to 9000,9000,900 (the LowRider's max_rate_mm_per_min).
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TMPDIR:-/tmp}/lrt_host_tests"
mkdir -p "$OUT"
export PATH="/c/msys64/ucrt64/bin:$PATH"
g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -I"$ROOT/lib/gcode/include" -I"$ROOT/src" \
    "$ROOT/tools/host_tests/log_replay.cpp" "$ROOT/src/services/segment_list.cpp" \
    "$ROOT/src/services/run_tracker.cpp" "$ROOT/src/services/time_left.cpp" "$ROOT/src/services/job_notes.cpp" \
    "$ROOT"/lib/gcode/src/*.cpp -o "$OUT/log_replay.exe"
"$OUT/log_replay.exe" "$@"
