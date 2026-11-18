#include "services/job_service.h"
#include "services/machine_service.h"
#include <Arduino.h>
#include <string.h>

JobService::Choice JobService::chosen = {false, FileService::CARD_CONTROLLER, "", "", 0};
JobService::Prep JobService::prepState = JobService::PREP_IDLE;
char JobService::prepErrorText[96] = "";

JobService::RunJob JobService::job = {"", "", -1};
JobService::RunState JobService::state = JobService::RUN_NONE;
uint32_t JobService::startMs = 0;
uint32_t JobService::endMs = 0;
uint32_t JobService::idleSinceMs = 0;
bool JobService::userPaused = false;
bool JobService::abortRequested = false;
char JobService::prompt[128] = "";
char JobService::endReason[128] = "";
float JobService::lastPercent = 0;

namespace {

// $SD/Run has this long to show up as a running file
const uint32_t START_TIMEOUT_MS = 8000;
// Idle with no SD: field for this long ends the job. FluidNC reports Idle
// during a G4 dwell, and drops SD: once it has read the last line.
const uint32_t END_IDLE_MS = 1500;
// Time left is not guessed before this much time and progress (D51)
const uint32_t LEFT_AFTER_MS = 60000;
const float LEFT_AFTER_PERCENT = 2.0f;

void copy(char *out, size_t outLen, const char *text) {
    strncpy(out, text, outLen - 1);
    out[outLen - 1] = '\0';
}

const char *baseName(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

}  // namespace

// --- The chosen file -----------------------------------------------------------

void JobService::choose(FileService::Card card, const char *folder, const FileService::Entry &entry) {
    chosen.set = true;
    chosen.card = card;
    FileService::join(chosen.path, sizeof(chosen.path), folder, entry.name);
    copy(chosen.name, sizeof(chosen.name), entry.name);
    chosen.size = entry.size;
    if (prepState == PREP_FAILED) prepState = PREP_IDLE;
}

void JobService::forget(FileService::Card card, const char *path) {
    if (chosen.set && chosen.card == card && strcmp(chosen.path, path) == 0) chosen.set = false;
}

// --- Use this job ----------------------------------------------------------------

void JobService::use() {
    if (!chosen.set || prepState == PREP_UPLOADING || prepState == PREP_CONFIRM_REPLACE) return;
    prepErrorText[0] = '\0';
    if (MachineService::jobActive() || active()) {
        prepState = PREP_FAILED;
        copy(prepErrorText, sizeof(prepErrorText), "Not while a job runs.");
        return;
    }
    if (chosen.card == FileService::CARD_CONTROLLER) {
        setReady(chosen.path, chosen.name, chosen.size);
        prepState = PREP_READY;
        return;
    }
    FileService::upload(chosen.path, false);
    prepState = PREP_UPLOADING;
}

void JobService::replace(bool yes) {
    if (prepState != PREP_CONFIRM_REPLACE) return;
    if (yes) {
        FileService::upload(chosen.path, true);
        prepState = PREP_UPLOADING;
    } else {
        FileService::cancelUpload();
        prepState = PREP_IDLE;
    }
}

void JobService::cancelUse() {
    FileService::cancelUpload();
    prepState = PREP_IDLE;
}

void JobService::prepTaken() {
    if (prepState == PREP_READY) prepState = PREP_IDLE;
}

void JobService::setReady(const char *path, const char *name, int32_t size) {
    copy(job.path, sizeof(job.path), path);
    copy(job.name, sizeof(job.name), name);
    job.size = size;
    enter(RUN_READY);
}

// --- Run ---------------------------------------------------------------------------------

void JobService::enter(RunState next) {
    if (next == state) return;
    Serial.printf("JobService: run state %d -> %d\n", (int)state, (int)next);
    uint32_t now = millis();
    if (next == RUN_STARTING || (next == RUN_RUNNING && !active())) {
        startMs = now;
        endMs = 0;
        abortRequested = false;
        userPaused = false;
        lastPercent = 0;
        endReason[0] = '\0';
    }
    if (next == RUN_FINISHED || next == RUN_ABORTED) endMs = now;
    if (next == RUN_PROMPT) {
        // The M0's (MSG ...) arrives when its line is read, before the hold
        copy(prompt, sizeof(prompt), MachineService::status().last_message);
    }
    idleSinceMs = 0;
    state = next;
}

void JobService::start() {
    if (state != RUN_READY) return;
    if (MachineService::runFile(job.path)) enter(RUN_STARTING);
}

void JobService::pause() {
    if (state != RUN_RUNNING) return;
    userPaused = true;
    MachineService::hold();
}

void JobService::resume() {
    if (state != RUN_PAUSED && state != RUN_PROMPT) return;
    // userPaused stays until FluidNC leaves the hold, so the panel doesn't
    // show a prompt for the moment between ~ and the next status report
    MachineService::resume();
}

void JobService::abort() {
    if (!active()) return;
    abortRequested = true;
    MachineService::reset();
}

void JobService::again() {
    if (state == RUN_FINISHED || state == RUN_ABORTED) enter(job.path[0] ? RUN_READY : RUN_NONE);
}

void JobService::dismiss() { again(); }

uint32_t JobService::elapsedMs() {
    if (state == RUN_NONE || state == RUN_READY) return 0;
    return (endMs ? endMs : millis()) - startMs;
}

float JobService::percent() { return state == RUN_FINISHED ? 100.0f : lastPercent; }

uint32_t JobService::leftMs() {
    float p = lastPercent;
    uint32_t e = elapsedMs();
    if (!active() || e < LEFT_AFTER_MS || p < LEFT_AFTER_PERCENT) return 0;
    return (uint32_t)(e * (100.0f - p) / p);
}

void JobService::poll() {
    // Use this job: follow the upload
    if (prepState == PREP_UPLOADING) {
        switch (FileService::uploadState()) {
            case FileService::UPLOAD_EXISTS:
                prepState = PREP_CONFIRM_REPLACE;
                break;
            case FileService::UPLOAD_DONE:
                setReady(FileService::uploadedPath(), chosen.name, (int32_t)FileService::uploadTotal());
                FileService::clearUpload();
                prepState = PREP_READY;
                break;
            case FileService::UPLOAD_FAILED:
                copy(prepErrorText, sizeof(prepErrorText), FileService::uploadError());
                FileService::clearUpload();
                prepState = PREP_FAILED;
                break;
            case FileService::UPLOAD_IDLE:
                prepState = PREP_IDLE;  // cancelled
                break;
            default:
                break;
        }
    }

    // While offline the job carries on (or not) on the controller; pick up
    // again once the status is back
    if (!MachineService::online()) {
        MachineService::setJobSession(active());
        return;
    }
    const FluidNCStatus &s = MachineService::status();
    uint32_t now = millis();
    if (s.is_sd_printing) lastPercent = s.sd_percent;

    switch (state) {
        case RUN_NONE:
        case RUN_READY:
        case RUN_FINISHED:
        case RUN_ABORTED:
            if (s.is_sd_printing) {
                // Started elsewhere, or a START whose state change was missed
                if (strcmp(baseName(s.sd_filename), job.name) != 0) {
                    copy(job.path, sizeof(job.path), s.sd_filename);
                    copy(job.name, sizeof(job.name), baseName(s.sd_filename));
                    job.size = -1;
                }
                enter(RUN_RUNNING);
                startMs = now - s.sd_elapsed_ms;
            }
            break;

        case RUN_STARTING:
            if (s.is_sd_printing || s.state == STATE_RUN || s.state == STATE_HOLD || s.state == STATE_HOME) {
                enter(RUN_RUNNING);
            } else if (s.state == STATE_ALARM) {
                snprintf(endReason, sizeof(endReason), "%s", s.alarm_message[0] ? s.alarm_message : "Alarm.");
                enter(RUN_ABORTED);
            } else if (now - startMs >= START_TIMEOUT_MS) {
                snprintf(endReason, sizeof(endReason), "The job did not start. %s", s.last_message);
                enter(RUN_ABORTED);
            }
            break;

        case RUN_RUNNING:
        case RUN_PROMPT:
        case RUN_PAUSED:
            if (s.state == STATE_ALARM) {
                if (abortRequested) {
                    copy(endReason, sizeof(endReason), "Aborted.");
                } else {
                    snprintf(endReason, sizeof(endReason), "%s", s.alarm_message[0] ? s.alarm_message : "Alarm.");
                }
                enter(RUN_ABORTED);
            } else if (s.state == STATE_HOLD || s.state == STATE_DOOR) {
                enter(userPaused || MachineService::heldByUser() ? RUN_PAUSED : RUN_PROMPT);
                // One prompt straight after another, between two status
                // reports: the text follows the newest M0's message
                if (state == RUN_PROMPT && strcmp(prompt, s.last_message) != 0) {
                    copy(prompt, sizeof(prompt), s.last_message);
                }
            } else if (!s.is_sd_printing && s.state == STATE_IDLE) {
                if (!idleSinceMs) {
                    idleSinceMs = now;
                } else if (now - idleSinceMs >= END_IDLE_MS) {
                    if (abortRequested) copy(endReason, sizeof(endReason), "Aborted.");
                    enter(abortRequested ? RUN_ABORTED : RUN_FINISHED);
                }
            } else {
                if (state != RUN_RUNNING) {
                    userPaused = false;
                    enter(RUN_RUNNING);
                }
                idleSinceMs = 0;
            }
            break;
    }
    MachineService::setJobSession(active());
}
