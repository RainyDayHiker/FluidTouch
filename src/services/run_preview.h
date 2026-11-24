#pragma once

#include <stdint.h>

// The running job's preview when Run has none (specs/05-file-workflow.md §2, 11-preview.md §2,
// D72, D94): a Controller SD job started elsewhere, or one running when the display restarted.
//
// Once per running job: its size from a listing of its folder (unless known), then the working
// folder's copy of that name and size is analysed without asking (unless another file is chosen
// on Job), or else Run offers the download with a confirm. Either way the running file becomes
// the chosen job. The offer lasts until the job ends.
class RunPreview {
public:
    enum State {
        NONE,      // nothing to offer: no job, a preview being followed, no display SD
        CHECKING,  // the controller is asked for the file's size
        OFFER,     // Run offers the download
        LOADING,   // the copy or the download is being fetched or analysed
    };
    static State state();
    // The file's size, -1 when the listing didn't give it
    static int32_t size();
    // Another file is chosen on Job: the download replaces it (the confirm says so)
    static bool otherChosen();
    static const char *otherName();
    // OFFER after a load of this file failed: why; "" otherwise
    static const char *failure();

    // The user confirmed: download now
    static void download();

    static void poll();
};
