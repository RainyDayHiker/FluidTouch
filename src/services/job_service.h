#ifndef LRT_JOB_SERVICE_H
#define LRT_JOB_SERVICE_H

#include "services/file_service.h"
#include <stdint.h>

// The chosen job and its run (specs/03-screens.md sections 3 and 4).
//
// Job:  choose() a file from the picker; use() makes it runnable: a file on
//       the controller is ready at once, a display SD file is uploaded first
//       (FileService). Then Run shows its start panel.
// Run:  READY -> STARTING -> RUNNING <-> PROMPT / PAUSED -> FINISHED | ABORTED.
//       A job started elsewhere (classic UI, FluidNC's web UI) shows as RUNNING.
class JobService {
public:
    // --- The chosen file ------------------------------------------------------------
    struct Choice {
        bool set;
        FileService::Card card;
        char path[FileService::PATH_LEN];
        char name[FileService::NAME_LEN];
        int32_t size;
    };
    static const Choice &choice() { return chosen; }
    static void choose(FileService::Card card, const char *folder, const FileService::Entry &entry);
    // A file was deleted: forget it if it was the chosen one
    static void forget(FileService::Card card, const char *path);

    // --- Use this job -----------------------------------------------------------------
    enum Prep { PREP_IDLE, PREP_UPLOADING, PREP_CONFIRM_REPLACE, PREP_FAILED, PREP_READY };
    static void use();
    static void replace(bool yes);   // answer to PREP_CONFIRM_REPLACE
    static void cancelUse();
    static Prep prep() { return prepState; }
    static const char *prepError() { return prepErrorText; }
    // Run has taken the job (PREP_READY seen): back to PREP_IDLE
    static void prepTaken();

    // --- Run --------------------------------------------------------------------------
    enum RunState { RUN_NONE, RUN_READY, RUN_STARTING, RUN_RUNNING, RUN_PROMPT, RUN_PAUSED, RUN_FINISHED, RUN_ABORTED };
    static RunState runState() { return state; }
    // Running, held, at a prompt, or starting
    static bool active() { return state >= RUN_STARTING && state <= RUN_PAUSED; }

    // The job Run shows: path on the controller, name, size (-1 if unknown)
    static const char *runPath() { return job.path; }
    static const char *runName() { return job.name; }
    static int32_t runSize() { return job.size; }

    static void start();     // READY: $SD/Run
    static void pause();     // RUNNING: feed hold
    static void resume();    // PAUSED or PROMPT: cycle start
    static void abort();     // soft reset
    static void again();     // FINISHED / ABORTED: back to READY with the same file
    static void dismiss();   // FINISHED / ABORTED: back to READY (or NONE)

    static uint32_t elapsedMs();
    // Rough time left from the SD: percent, or 0 while unknown (D51)
    static uint32_t leftMs();
    static float percent();
    // The M0 message at a prompt
    static const char *promptText() { return prompt; }
    // Why the job ended, for ABORTED
    static const char *endText() { return endReason; }

    // Call from the main loop
    static void poll();

private:
    struct RunJob {
        char path[FileService::PATH_LEN];
        char name[FileService::NAME_LEN];
        int32_t size;
    };
    static void setReady(const char *path, const char *name, int32_t size);
    static void enter(RunState next);

    static Choice chosen;
    static Prep prepState;
    static char prepErrorText[96];

    static RunJob job;
    static RunState state;
    static uint32_t startMs;
    static uint32_t endMs;
    static uint32_t idleSinceMs;
    static bool userPaused;
    static bool abortRequested;
    static char prompt[128];
    static char endReason[128];
    static float lastPercent;
};

#endif // LRT_JOB_SERVICE_H
