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

    // The operation the tool is in, 1-based (the last one it was in between operations); 0 before
    // the first, or when the file has no structure (D92)
    static int op();
    // The message to show for the controller's last one (D65, D92): while the tool is followed,
    // a message from the file is replaced by the file's message at the tool's line; anything else
    // (the controller's own messages, a prompt) is shown as it is
    static const char *message(const char *controllerMessage);
    // Time left comes from the plan (11 §7, D91): following a running job whose list is whole
    static bool plansTime();
    // That time left in ms; 0 until the first match
    static uint32_t leftMs();
    // The operation's time left in ms (D100), as leftMs() counts it, to the end of op()'s last
    // move; 0 when the time isn't planned, or the tool is between operations
    static uint32_t opLeftMs();

    static void poll();
};
