#include "services/run_follow.h"
#include "services/job_service.h"
#include "services/machine_service.h"
#include "services/preview_service.h"
#include "services/run_tracker.h"
#include "services/time_left.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace {

RunTracker tracker;
uint32_t generation = 0;
uint32_t lastReport = 0;
bool wasActive = false;
JobService::RunState lastState = JobService::RUN_NONE;
bool isFollowing = false;
TimeLeft timeLeft;
int lastOp = 0;
float lastFeedOverride = 100, lastRapidOverride = 100;
uint32_t lastEstimateGen = 0;
int lastSource = -1;  // how the last match was found, for the log: 1 Ln:, 0 the position
// The end of lastOp's last move in the plan (D100): planned seconds of feeds and of the rest
int opEndFor = 0;  // the op they were looked for; 0 none
bool opEndFound = false;
uint32_t opEndGen = 0;
double opEndFeed = 0, opEndOther = 0;
bool inOp = false;  // the matched move is in lastOp (not between operations)

// The read position is rounded to a hundredth of a percent: a little slack past it
const uint64_t READ_SLACK = 1024;

bool previewIsRunJob() {
    if (!PreviewService::segments()) return false;
    if (JobService::runSize() < 0 || JobService::runSize() != PreviewService::jobSize()) return false;
    return strcmp(JobService::runName(), PreviewService::jobName()) == 0;
}

// Planned seconds before `record`: the checkpoint's, then the moves from there (at most 256)
TimeLeft::Plan planAt(const SegmentList &list, uint32_t record) {
    const PreviewService::Summary &sum = PreviewService::summary();
    TimeLeft::Plan p = {0, 0, 0, 0, sum.estimateFeedSeconds, sum.estimateSeconds - sum.estimateFeedSeconds};
    if (!list.checkpoints()) return p;
    uint32_t cp = record / SegmentList::CHECKPOINT_EVERY;
    if (cp >= list.checkpoints()) cp = list.checkpoints() - 1;
    double total = list.checkpoint(cp).seconds, feed = list.checkpoint(cp).feedSeconds;
    SegmentList::Rates rates = PreviewService::rates();
    SegmentList::Reader reader(list, cp);
    SegmentList::Move m;
    bool incomplete = false;
    bool more;
    while ((more = reader.next(m)) && m.record < record) {
        double s = SegmentList::moveSeconds(m, rates, incomplete);
        total += s;
        feed += SegmentList::feedPart(m, s);
    }
    p.feedDone = p.feedAfter = feed;
    p.otherDone = p.otherAfter = total - feed;
    if (more && m.record == record) {
        // The matched move itself
        double s = SegmentList::moveSeconds(m, rates, incomplete);
        p.feedAfter += SegmentList::feedPart(m, s);
        p.otherAfter += s - SegmentList::feedPart(m, s);
    }
    return p;
}

// The planned seconds at the end of operation `op`'s last move, reading on from `record` (the
// matched move) until a later operation starts; false when no move of `op` is found
bool opEndAt(const SegmentList &list, const JobNotes &notes, int op, uint32_t record, double &feedEnd,
             double &otherEnd) {
    if (!list.checkpoints()) return false;
    uint32_t cp = record / SegmentList::CHECKPOINT_EVERY;
    if (cp >= list.checkpoints()) cp = list.checkpoints() - 1;
    double total = list.checkpoint(cp).seconds, feed = list.checkpoint(cp).feedSeconds;
    SegmentList::Rates rates = PreviewService::rates();
    SegmentList::Reader reader(list, cp);
    SegmentList::Move m;
    bool incomplete = false, found = false;
    while (reader.next(m)) {
        double s = SegmentList::moveSeconds(m, rates, incomplete);
        total += s;
        feed += SegmentList::feedPart(m, s);
        if (m.record < record) continue;
        int at = notes.opAt(m.line);
        if (at == op) {
            found = true;
            feedEnd = feed;
            otherEnd = total - feed;
        } else if (at > op) {
            break;
        }
    }
    return found;
}

double runSeconds() { return JobService::runningMs() / 1000.0; }

}  // namespace

bool RunFollow::following() { return isFollowing; }
int RunFollow::op() { return isFollowing ? lastOp : 0; }

bool RunFollow::plansTime() {
    const SegmentList *list = PreviewService::segments();
    return isFollowing && JobService::active() && list && !list->truncated();
}

uint32_t RunFollow::leftMs() {
    if (!plansTime() || !timeLeft.known()) return 0;
    return (uint32_t)(timeLeft.seconds() * 1000.0);
}

uint32_t RunFollow::opLeftMs() {
    if (!plansTime() || !timeLeft.known() || !inOp || !lastOp || opEndFor != lastOp || !opEndFound) return 0;
    double s = timeLeft.leftUntil(opEndFeed, opEndOther, runSeconds());
    return (uint32_t)(fmax(1, s) * 1000.0);
}

const char *RunFollow::message(const char *controllerMessage) {
    if (!isFollowing || !JobService::active() || JobService::runState() == JobService::RUN_PROMPT) {
        return controllerMessage;
    }
    const JobNotes *notes = PreviewService::notes();
    if (!notes || !tracker.matched()) return controllerMessage;
    // Whether the controller's message is one of the file's, looked up once per message
    static char checked[sizeof(((FluidNCStatus *)nullptr)->last_message)] = "";
    static bool fromFile = false;
    static const JobNotes *checkedNotes = nullptr;
    if (strcmp(checked, controllerMessage) != 0 || checkedNotes != notes) {
        strncpy(checked, controllerMessage, sizeof(checked) - 1);
        checked[sizeof(checked) - 1] = '\0';
        checkedNotes = notes;
        fromFile = notes->isFileMessage(controllerMessage);
    }
    if (!fromFile) return controllerMessage;
    const char *atTool = notes->messageAt(tracker.line());
    return atTool ? atTool : "";
}
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
        timeLeft.reset();
        lastOp = 0;
        opEndFor = 0;
        opEndFound = false;
        inOp = false;
        lastSource = -1;
        generation++;
    }
    wasActive = active;
    lastState = state;
    bool ended = state == JobService::RUN_FINISHED || state == JobService::RUN_ABORTED;
    isFollowing = (active || ended || state == JobService::RUN_READY) && previewIsRunJob();
    if (!isFollowing || !active) return;
    timeLeft.tick(runSeconds());
    if (!MachineService::online()) return;

    const FluidNCStatus &s = MachineService::status();
    const SegmentList &list = *PreviewService::segments();
    // New axis rates made the estimate again: the same position in its new numbers
    if (PreviewService::estimateGeneration() != lastEstimateGen) {
        lastEstimateGen = PreviewService::estimateGeneration();
        if (tracker.matched()) timeLeft.replan(planAt(list, tracker.record()));
    }
    if (s.last_update_ms == lastReport) return;
    lastReport = s.last_update_ms;
    if (state == JobService::RUN_STARTING) return;  // still where it was before START

    uint64_t read = RunTracker::READ_ALL;
    if (s.is_sd_printing) {
        read = (uint64_t)((double)s.sd_percent / 100.0 * (double)PreviewService::jobSize()) + READ_SLACK;
    }
    bool moved = tracker.update(list, s.wpos_x, s.wpos_y, s.wpos_z, read, s.line_number);
    if (!tracker.matched()) return;
    if (moved && (int)tracker.byLineNumber() != lastSource) {
        lastSource = tracker.byLineNumber();
        Serial.printf("RunFollow: following by %s (line %lu)\n", lastSource ? "Ln:" : "position",
                      (unsigned long)tracker.line() + 1);
    }
    bool overrides = s.feed_override != lastFeedOverride || s.rapid_override != lastRapidOverride;
    if (moved || overrides || !timeLeft.known()) {
        lastFeedOverride = s.feed_override;
        lastRapidOverride = s.rapid_override;
        timeLeft.match(runSeconds(), planAt(list, tracker.record()), s.feed_override / 100.0,
                       s.rapid_override / 100.0);
    }
    if (moved) {
        const JobNotes *notes = PreviewService::notes();
        int op = notes ? notes->opAt(tracker.line()) : 0;
        if (op) lastOp = op;
        inOp = op != 0;
    }
    // Where the operation ends in the plan: once per operation, and again with a new estimate
    if (inOp && lastOp && (opEndFor != lastOp || opEndGen != lastEstimateGen)) {
        const JobNotes *notes = PreviewService::notes();
        opEndFound = notes && opEndAt(list, *notes, lastOp, tracker.record(), opEndFeed, opEndOther);
        opEndFor = lastOp;
        opEndGen = lastEstimateGen;
    }
}
