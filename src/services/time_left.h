#pragma once

#include <stdint.h>

// Time left from the file's estimate (specs/11-preview.md §7, D91). Plain C++ for the host test.
//
// At each match: the plan still to come (feeds over the feed override, the rest over the rapid
// override) x a correction factor, which settles from 1 toward the running time over the planned
// time of the part done (each part over the overrides it ran at), from CORRECT_AFTER_S of running
// on. The target counts down with the running time from the match, but not below the plan after
// the matched move (a long move taking longer than planned). The value shown counts down too and
// moves toward the target a part of the way at a time; running time stands still at a prompt or a
// pause, and so does the value.
//
// Probes and homings (D101) have fixed allowances in the plan (`workAhead`, `workAfter`), added as
// they are; the factor leaves out the running time and the plan across one (`setAside` at a match).
class TimeLeft {
public:
    static constexpr double CORRECT_AFTER_S = 120;  // §7: 2 minutes of cutting
    static constexpr double FACTOR_TAU_S = 60;      // the factor's settling time
    static constexpr double SHOW_TAU_S = 20;        // the shown value's settling time toward the target
    static constexpr double FACTOR_MIN = 0.5, FACTOR_MAX = 5;

    // Planned seconds at 100%: before the matched move, after it, and in the whole job; feeds and
    // arcs (without dwells), and the rest (rapids, dwells)
    struct Plan {
        double feedDone, otherDone;
        double feedAfter, otherAfter;
        double feedTotal, otherTotal;
        // Allowances (seconds, as they are) of the probes and homings after the matched move's line,
        // and after the next planned move's line (those in between are being done now)
        double workAhead, workAfter;
    };

    void reset();
    // A match (the first, a new one, or new overrides): `runS` the running time in seconds,
    // scales 1 = 100%. `setAside`: a probe or homing since the last match, so the running time and
    // the plan since then are left out of the factor
    void match(double runS, const Plan &plan, double feedScale, double rapidScale, bool setAside = false);
    // The estimate was made again (new axis rates): the same position in new numbers
    void replan(const Plan &plan);
    // Each loop
    void tick(double runS);

    bool known() const { return haveShown; }
    double seconds() const { return shown; }
    double factor() const { return k; }
    // The time to a point further on, as the target counts it: the plan from the matched move to
    // the point (`feedEnd`, `otherEnd`: planned seconds at 100% from the start of the job), over the
    // overrides, x the factor, counted down from the match but not below the plan after the matched
    // move. Not smoothed. 0 before the first match (an operation's time left, D100)
    double leftUntil(double feedEnd, double otherEnd, double runS) const;

private:
    // The plan from (feed, other) to the end, over the overrides
    double remainingFrom(double feed, double other) const;
    double target(double runS) const;

    bool haveMatch = false;
    Plan last = {};
    double feedScale = 1, rapidScale = 1;
    double doneScaled = 0;  // planned seconds of the part done, each part over its overrides
    double setAsideRun = 0;  // running time across probes and homings, not in the factor
    double k = 1, kTarget = 1;
    double matchRun = 0;  // running time at the last match
    bool haveShown = false;
    double shown = 0;
    double shownRun = 0;
};
