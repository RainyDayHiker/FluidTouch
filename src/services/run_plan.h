#pragma once

#include "services/job_notes.h"
#include "services/segment_list.h"
#include "services/time_left.h"

// Where the matched move is in the plan, for TimeLeft (specs/11-preview.md §7): RunFollow's, the
// host test's and the log replay's. Plain C++ with no Arduino calls.
namespace RunPlan {

// Planned seconds before the matched `record`, after it, and in the whole job (`estimate` from
// SegmentList::estimate); with `notes`, the probes' and homings' allowances still ahead (D101)
TimeLeft::Plan at(const SegmentList &list, const JobNotes *notes, uint32_t record, const SegmentList::Rates &rates,
                  double estimateSeconds, double estimateFeedSeconds);

// Planned seconds at the end of operation `op`'s last move (D100), reading on from `record` until
// a later operation starts; false when no move of `op` is found
bool opEnd(const SegmentList &list, const JobNotes &notes, int op, uint32_t record, const SegmentList::Rates &rates,
           double &feedEnd, double &otherEnd);

}  // namespace RunPlan
