#pragma once

#include <stdint.h>

// Follows the running job on its preview (specs/11-preview.md §6, D79, D89): when the
// preview's file is the running one (same name and size), each status report moves the match
// (RunTracker) and Run draws the moves before it as done. Polled from the main loop, so it
// keeps up while another screen is shown.
class RunFollow {
public:
    // The preview is of the job on Run (ready to start, running, or just ended)
    static bool following();
    // The moves before this record are done; at the end of a finished job, all of them
    static uint32_t doneRecord();
    // The matched move's line (0-based); valid when matched()
    static bool matched();
    static uint32_t line();
    // Moves on with each new run (START, Run again, a job started elsewhere): time to redraw
    static uint32_t runGeneration();

    static void poll();
};
