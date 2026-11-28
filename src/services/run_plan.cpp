#include "services/run_plan.h"

namespace RunPlan {

TimeLeft::Plan at(const SegmentList &list, const JobNotes *notes, uint32_t record, const SegmentList::Rates &rates,
                  double estimateSeconds, double estimateFeedSeconds) {
    TimeLeft::Plan p = {0, 0, 0, 0, estimateFeedSeconds, estimateSeconds - estimateFeedSeconds, 0, 0};
    if (!list.checkpoints()) return p;
    // The checkpoint's seconds, then the moves from there (at most 256)
    uint32_t cp = record / SegmentList::CHECKPOINT_EVERY;
    if (cp >= list.checkpoints()) cp = list.checkpoints() - 1;
    double total = list.checkpoint(cp).seconds, feed = list.checkpoint(cp).feedSeconds;
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
        if (notes) {
            // Probes and homings after its line; those before the next move are being done now
            p.workAhead = notes->workSecondsAfter(m.line);
            SegmentList::Move next;
            p.workAfter = reader.next(next) ? notes->workSecondsAfter(next.line) : 0;
        }
    }
    return p;
}

bool opEnd(const SegmentList &list, const JobNotes &notes, int op, uint32_t record, const SegmentList::Rates &rates,
           double &feedEnd, double &otherEnd) {
    if (!list.checkpoints()) return false;
    uint32_t cp = record / SegmentList::CHECKPOINT_EVERY;
    if (cp >= list.checkpoints()) cp = list.checkpoints() - 1;
    double total = list.checkpoint(cp).seconds, feed = list.checkpoint(cp).feedSeconds;
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

}  // namespace RunPlan
