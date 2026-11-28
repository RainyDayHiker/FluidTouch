#include "services/time_left.h"
#include <math.h>

void TimeLeft::reset() { *this = TimeLeft(); }

double TimeLeft::remainingFrom(double feed, double other) const {
    return fmax(0, last.feedTotal - feed) / feedScale + fmax(0, last.otherTotal - other) / rapidScale;
}

double TimeLeft::target(double runS) const {
    double fromMatch = remainingFrom(last.feedDone, last.otherDone) * k + last.workAhead - (runS - matchRun);
    double afterMove = remainingFrom(last.feedAfter, last.otherAfter) * k + last.workAfter;
    return fmax(fromMatch, afterMove);
}

double TimeLeft::leftUntil(double feedEnd, double otherEnd, double runS) const {
    if (!haveMatch) return 0;
    auto span = [&](double feed, double other) {
        return fmax(0, feedEnd - feed) / feedScale + fmax(0, otherEnd - other) / rapidScale;
    };
    double fromMatch = span(last.feedDone, last.otherDone) * k - (runS - matchRun);
    double afterMove = span(last.feedAfter, last.otherAfter) * k;
    return fmax(0, fmax(fromMatch, afterMove));
}

void TimeLeft::match(double runS, const Plan &plan, double fs, double rs, bool setAside) {
    if (haveMatch && setAside) {
        // A probe or homing since the last match: neither its time nor the plan around it
        setAsideRun += fmax(0, runS - matchRun);
    } else if (haveMatch) {
        // The part done since the last match ran at the overrides of then
        doneScaled += fmax(0, plan.feedDone - last.feedDone) / feedScale +
                      fmax(0, plan.otherDone - last.otherDone) / rapidScale;
    } else {
        doneScaled = plan.feedDone + plan.otherDone;
    }
    haveMatch = true;
    last = plan;
    feedScale = fs > 0.01 ? fs : 0.01;
    rapidScale = rs > 0.01 ? rs : 0.01;
    matchRun = runS;
    double cutting = runS - setAsideRun;
    if (cutting >= CORRECT_AFTER_S && doneScaled > 1) {
        kTarget = fmin(FACTOR_MAX, fmax(FACTOR_MIN, cutting / doneScaled));
    }
    if (!haveShown) {
        shown = fmax(1, target(runS));
        shownRun = runS;
        haveShown = true;
    }
}

void TimeLeft::replan(const Plan &plan) {
    if (!haveMatch) return;
    double before = last.feedDone + last.otherDone, after = plan.feedDone + plan.otherDone;
    if (before > 0) doneScaled *= after / before;
    last = plan;
}

void TimeLeft::tick(double runS) {
    if (!haveShown) return;
    double dt = runS - shownRun;
    if (dt <= 0) return;  // waiting: a prompt or a pause
    shownRun = runS;
    k += (kTarget - k) * (1 - exp(-dt / FACTOR_TAU_S));
    shown -= dt;
    shown += (target(runS) - shown) * (1 - exp(-dt / SHOW_TAU_S));
    if (shown < 1) shown = 1;
}
