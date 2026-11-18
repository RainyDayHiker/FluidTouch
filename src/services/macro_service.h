#ifndef LRT_MACRO_SERVICE_H
#define LRT_MACRO_SERVICE_H

#include <stdint.h>

// The 12 macro slots and the macro that runs (specs/03-screens.md section 5,
// D59 to D61). The list lives in RAM, read once from the lrt key "macros" and
// written back by save() only when it changed (D44).
//
// Run: STARTING -> RUNNING <-> HELD -> DONE | STOPPED, then NONE after a while.
// A macro file started elsewhere (classic UI, web UI) shows as RUNNING.
class MacroService {
public:
    static const int SLOTS = 12;
    static const int NAME_LEN = 24;   // 23 characters
    static const int TEXT_LEN = 96;   // 95 characters
    static const int COLORS = 8;      // UITheme::MACRO_COLOR_1..8

    enum Kind : uint8_t { KIND_EMPTY, KIND_FILE, KIND_COMMAND };

    // A file macro's text is its file name in FOLDER; a command's is the line
    struct Macro {
        char name[NAME_LEN];
        char text[TEXT_LEN];
        uint8_t kind;
        uint8_t color;
    };

    // The classic UI's macro folder on the controller's SD (D13)
    static const char *const FOLDER;
    // Is this controller path a macro file (any file under /fluidtouch/macros/)?
    static bool isMacroPath(const char *path);

    // --- The list -----------------------------------------------------------------
    static const Macro &get(int slot);
    static int count();  // slots in use
    static void set(int slot, const Macro &macro);
    static void clear(int slot);
    static void swap(int a, int b);
    // Writes the list if it changed since it was read or last saved. Returns
    // true if it wrote.
    static bool save();

    // --- Running ------------------------------------------------------------------
    enum RunState { RUN_NONE, RUN_STARTING, RUN_RUNNING, RUN_HELD, RUN_DONE, RUN_STOPPED };
    static RunState runState() { return state; }
    static bool active() { return state >= RUN_STARTING && state <= RUN_HELD; }
    // Idle, no job, no macro (as Zero)
    static bool canRun();
    static bool run(int slot);
    static void resume();  // HELD: cycle start
    static void stop();    // soft reset
    static const char *runName() { return name; }
    static bool runIsFile() { return isFile; }
    static float percent();
    // Why it stopped, for STOPPED
    static const char *endText() { return endReason; }
    // The controller's last message, once one has come since the macro
    // started; "" before that
    static const char *message();

    // Call from the main loop
    static void poll();

private:
    static void load();
    static void enter(RunState next);

    static Macro macros[SLOTS];
    static bool loaded;
    static bool changed;

    static RunState state;
    static char name[NAME_LEN + 8];
    static bool isFile;
    static bool stopRequested;
    static uint32_t sinceMs;      // when the current state began
    static uint32_t idleSinceMs;  // when the controller was first seen idle, or 0
    static float lastPercent;
    static char endReason[128];
    static char startMessage[128];  // the last message when the macro started
    static bool messageSeen;        // a new one has come since
};

#endif // LRT_MACRO_SERVICE_H
