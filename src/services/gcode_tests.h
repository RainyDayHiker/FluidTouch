#pragma once

// The G-code library's device test suite (specs/04 §2, D69, D73), run from Settings > Tools.
// The runner (services/gcode_device/, copied from the CNC-Tools release) reads its data from
// /gcode-tests/ on the display SD and writes one line per case to /gcode-tests/results.txt and
// the serial port. It runs in a background task, so the screen and the connection keep going.
namespace GcodeTests {

constexpr const char *DATA_FOLDER = "/gcode-tests";

enum class State : unsigned char { Idle, Running, Done, Error };

// Starts the suite. False, with the reason in statusText(), when the card or the data is
// missing, a job is running, memory is short, or it is already running.
bool start();

State state();

// One line for Settings: what is needed, progress, or the summary of the last run.
// Copied out under a lock, so it is safe to call from the UI while the suite runs.
void statusText(char *out, unsigned size);

// The first failing case of the last run, or "" (for showing under the summary).
void firstFailure(char *out, unsigned size);

}  // namespace GcodeTests
