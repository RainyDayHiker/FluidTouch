#include "services/run_follow.h"
#include "services/job_service.h"
#include "services/machine_service.h"
#include "services/preview_service.h"
#include "services/run_tracker.h"
#include <string.h>

namespace {

RunTracker tracker;
uint32_t generation = 0;
uint32_t lastReport = 0;
bool wasActive = false;
JobService::RunState lastState = JobService::RUN_NONE;
bool isFollowing = false;

// The read position is rounded to a hundredth of a percent: a little slack past it
const uint64_t READ_SLACK = 1024;

bool previewIsRunJob() {
    if (!PreviewService::segments()) return false;
    if (JobService::runSize() < 0 || JobService::runSize() != PreviewService::jobSize()) return false;
    return strcmp(JobService::runName(), PreviewService::jobName()) == 0;
}

}  // namespace

bool RunFollow::following() { return isFollowing; }
bool RunFollow::matched() { return isFollowing && tracker.matched(); }
uint32_t RunFollow::line() { return tracker.line(); }
uint32_t RunFollow::runGeneration() { return generation; }

uint32_t RunFollow::doneRecord() {
    if (!isFollowing) return 0;
    if (JobService::runState() == JobService::RUN_FINISHED) return PreviewService::segments()->records();
    return tracker.matched() ? tracker.record() : 0;
}

void RunFollow::poll() {
    JobService::RunState state = JobService::runState();
    bool active = JobService::active();
    // A new run, or back to the start panel (Run again, a new job): nothing done yet
    if ((active && !wasActive) || (state == JobService::RUN_READY && lastState != JobService::RUN_READY)) {
        tracker.reset();
        generation++;
    }
    wasActive = active;
    lastState = state;
    bool ended = state == JobService::RUN_FINISHED || state == JobService::RUN_ABORTED;
    isFollowing = (active || ended || state == JobService::RUN_READY) && previewIsRunJob();
    if (!isFollowing || !active || !MachineService::online()) return;

    const FluidNCStatus &s = MachineService::status();
    if (s.last_update_ms == lastReport) return;
    lastReport = s.last_update_ms;
    if (state == JobService::RUN_STARTING) return;  // still where it was before START

    uint64_t read = RunTracker::READ_ALL;
    if (s.is_sd_printing) {
        read = (uint64_t)((double)s.sd_percent / 100.0 * (double)PreviewService::jobSize()) + READ_SLACK;
    }
    tracker.update(*PreviewService::segments(), s.wpos_x, s.wpos_y, s.wpos_z, read);
}
