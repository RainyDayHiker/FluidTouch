#pragma once

#include "services/file_service.h"
#include "services/segment_list.h"
#include <stdint.h>

// The chosen job's preview data (specs/11-preview.md §2, part 5a): a Controller SD file is
// downloaded into the working folder /lrt/work/ on the display SD, then the job is analysed with
// the G-code library (with the outline, D75, D77), both on the worker task. A display SD file is
// analysed where it is (D82). The analysis packs the segments into the segment list (§3) for
// the drawing (part 5b). Nothing here blocks the main loop; the screens poll.
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

    // What the analysis found, copied out of the library's result. Positions in mm (inch
    // files converted); `inch` says which units the info line shows
    struct Summary {
        uint32_t lines;
        bool inch;            // the file's primary units
        bool hasExtents;
        double minX, maxX, minY, maxY;  // displayExtents, in the file's units
        bool outline;         // the Outline Check is there
        uint32_t fetchMs;     // 0 for a display SD file
        uint32_t analyseMs;
        uint32_t arenaPeak;   // bytes

        // The info line and the drawing (specs/11-preview.md §4, §5)
        bool hasJob;  // jobExtents: what Fit shows
        double jobMinX, jobMaxX, jobMinY, jobMaxY;
        bool hasStock;  // the stock box
        double stockMinX, stockMaxX, stockMinY, stockMaxY;
        bool hasProbe;  // the probe point (fixedMoves, kind Probe)
        float probeX, probeY;
        bool hasZ;
        float zMin, zMax;
        char tools[48];  // "T1 T2", or empty
        int ops;         // enabled operations, -1 without a structure
        bool unitsAssumed, unitsMixed, skipped;
        uint8_t shownPercent;  // 100, or how much of the job the list holds
        uint32_t moves;
        double estimateSeconds;
        bool estimateIncomplete;
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
    // READY: the packed segments; otherwise null
    static const SegmentList *segments();
    // Moves on each time a job becomes READY: time to draw it (a new estimate doesn't move it)
    static uint32_t readyGeneration();

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
