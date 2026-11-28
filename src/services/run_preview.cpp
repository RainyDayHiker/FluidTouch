#include "services/run_preview.h"
#include "services/file_service.h"
#include "services/job_service.h"
#include "services/preview_service.h"
#include "services/run_follow.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>

namespace {

RunPreview::State current = RunPreview::NONE;
char runKey[FileService::PATH_LEN] = "";  // the running job these flags are for
bool checked = false;                     // its size asked for (or known)
int32_t fileSize = -1;
bool copyTried = false;                   // its working folder copy tried once
int32_t nameCopySize = -1;                // a copy only its name vouches for (the size unchecked, D98)

void copy(char *out, size_t outLen, const char *text) {
    strncpy(out, text, outLen - 1);
    out[outLen - 1] = '\0';
}

void reset() {
    current = RunPreview::NONE;
    runKey[0] = '\0';
    checked = false;
    fileSize = -1;
    copyTried = false;
    nameCopySize = -1;
}

// The preview service is on the running file
bool previewOnRunFile() { return strcmp(PreviewService::jobPath(), JobService::runPath()) == 0; }

// The size of the working folder's file of the running file's name (from before a restart or a
// UI switch); -1 when there is none
int32_t copySize() {
    if (FileService::displayCardBusy()) return -1;
    char path[FileService::PATH_LEN];
    FileService::join(path, sizeof(path), PreviewService::WORK_FOLDER, JobService::runName());
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    int64_t size = f.isDirectory() ? -1 : (int64_t)f.size();
    f.close();
    return size > INT32_MAX ? -1 : (int32_t)size;
}

}  // namespace

RunPreview::State RunPreview::state() { return current; }
int32_t RunPreview::size() { return fileSize; }

bool RunPreview::otherChosen() {
    const JobService::Choice &c = JobService::choice();
    return c.set && !(c.card == FileService::CARD_CONTROLLER && strcmp(c.path, JobService::runPath()) == 0);
}

const char *RunPreview::otherName() { return JobService::choice().name; }

const char *RunPreview::failure() {
    if (current != OFFER || !previewOnRunFile()) return "";
    PreviewService::State s = PreviewService::state();
    return s == PreviewService::FAILED ? PreviewService::message() : "";
}

int32_t RunPreview::nameCopy() { return current == OFFER ? nameCopySize : -1; }

const char *RunPreview::sizeError() {
    return current == OFFER && checked && fileSize < 0 ? FileService::sizeError() : "";
}

void RunPreview::useCopy() {
    if (current != OFFER || nameCopySize < 0) return;
    Serial.printf("RunPreview: %s: the working folder's copy used by name, %ld bytes\n", JobService::runPath(),
                  (long)nameCopySize);
    JobService::chooseRunning(nameCopySize, false);
    current = LOADING;
}

void RunPreview::download() {
    if (current != OFFER) return;
    Serial.printf("RunPreview: downloading %s during the cut\n", JobService::runPath());
    JobService::chooseRunning(fileSize, true);
    current = LOADING;
}

void RunPreview::poll() {
    const char *path = JobService::runPath();
    if (!JobService::active() || strncmp(path, "/sd/", 4) != 0) {
        if (runKey[0]) reset();
        return;
    }
    if (strcmp(path, runKey) != 0) {
        reset();
        copy(runKey, sizeof(runKey), path);
    }

    PreviewService::State ps = PreviewService::state();
    if (previewOnRunFile()) {
        // A size learnt from the analysed file (a job started elsewhere, listing failed)
        if (ps == PreviewService::READY && JobService::runSize() < 0) {
            JobService::learnRunSize(path, PreviewService::jobSize());
        }
        if (ps == PreviewService::FETCHING || ps == PreviewService::ANALYSING) {
            current = LOADING;
            return;
        }
    }
    if (RunFollow::following() || !FileService::displayCardReady()) {
        current = NONE;
        return;
    }

    if (!checked) {
        if (JobService::runSize() >= 0) {
            fileSize = JobService::runSize();
        } else if (FileService::sizeState() == FileService::SIZE_LOADING &&
                   strcmp(FileService::sizePath(), path) == 0) {
            current = CHECKING;
            return;
        } else if (current != CHECKING || strcmp(FileService::sizePath(), path) != 0) {
            FileService::sizeOf(path);
            current = CHECKING;
            return;
        } else {
            fileSize = FileService::sizeState() == FileService::SIZE_KNOWN ? FileService::sizeResult() : -1;
        }
        checked = true;
        JobService::learnRunSize(path, fileSize);
    }

    // The copy from before, used without asking when its size matches and no other file is chosen.
    // When the size couldn't be checked (the controller refused the listing during the cut), the
    // copy is offered beside the download, for the operator to judge (D98)
    if (!copyTried) {
        copyTried = true;
        int32_t local = copySize();
        if (local >= 0 && local == fileSize && !otherChosen()) {
            Serial.printf("RunPreview: %s: the working folder's copy matches, %ld bytes\n", path, (long)fileSize);
            JobService::chooseRunning(fileSize, false);
            current = LOADING;
            return;
        }
        nameCopySize = fileSize < 0 ? local : -1;
        if (nameCopySize >= 0) {
            Serial.printf("RunPreview: %s: size unchecked (%s); a copy of that name, %ld bytes, is offered\n", path,
                          FileService::sizeError(), (long)nameCopySize);
        }
    }
    current = OFFER;
}
