#pragma once

#include "services/file_service.h"
#include <stdint.h>

// The chosen job's preview data (specs/11-preview.md §2, part 5a): a Controller SD file is
// downloaded into the working folder /lrt/work/ on the display SD, then the job is analysed with
// the G-code library (with the outline, D75, D77), both on the worker task. A display SD file is
// analysed where it is (D82). Nothing here blocks the main loop; the screens poll.
//
//   load() -> [WAITING_FOR_JOB] -> FETCHING -> ANALYSING -> READY | FAILED | CANCELLED
//   No display SD card: NO_CARD (no preview, D82).
class PreviewService {
public:
    enum State {
        NONE,             // no job chosen
        NO_CARD,          // no display SD: the job can be run, not previewed
        WAITING_FOR_JOB,  // a Controller SD file chosen during a cut: fetched when it ends
        FETCHING,
        ANALYSING,
        READY,
        FAILED,     // message() says why; retry() tries again
        CANCELLED,  // retry() starts again
    };

    // What the analysis found, copied out of the library's result
    struct Summary {
        uint32_t lines;
        bool inch;            // the file's primary units
        bool hasExtents;
        double minX, maxX, minY, maxY;  // displayExtents, in the file's units
        bool outline;         // the Outline Check is there
        uint32_t fetchMs;     // 0 for a display SD file
        uint32_t analyseMs;
        uint32_t arenaPeak;   // bytes
    };

    // A file was chosen (JobService::choose): starts over for it
    static void load(FileService::Card card, const char *path, const char *name, int32_t size);
    // Nothing chosen any more
    static void clear();
    static void cancel();
    static void retry();

    static State state();
    // 0..1 for the bar while FETCHING or ANALYSING
    static float progress();
    // FAILED: why, in a sentence
    static const char *message();
    static const Summary &summary();

    // The worker task is running (fetch or analysis)
    static bool busy();
    // The library tests need the memory: drops the preview (CANCELLED), false while busy
    static bool releaseMemory();

    // The working folder on the display SD
    static const char *const WORK_FOLDER;

    // Call from the main loop
    static void poll();

private:
    static void start();
};
