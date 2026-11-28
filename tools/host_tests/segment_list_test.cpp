// Host test of the segment list (specs/11-preview.md §10): every file given is analysed with the
// G-code library; the library's segments are compared with the packed records read back.
//
//   tools/host_tests/run.sh <file or folder>...
//
// Checked for each move: kind, line, end X/Y within one step and Z within one Z step (repacks
// round twice), an arc's centre and sweep; that a reader started at any checkpoint reads the same
// moves as one from the start; and a job larger than 327 mm doubles the step.
//
// RunTracker (§6): the tool is put at the middle of each move in turn; the match must never go
// back and never pass the move the tool is on. How often it is that
// very move is printed ("follow"): less than 100% where a path passes within the tolerance of
// an earlier one still in the window (the match stays on the earlier pass). For a file whose N
// words increase ("N"), each move's N must be the one on its line, and a report with that `Ln:`
// (and a position far off) must match that very move; printed "Ln" with the moves checked.
//
// JobNotes (D92): the file's messages come in line order, each is recognised as FluidNC sends
// it (as written, or with the comment's spaces dropped and shifted by one), and a message of the
// controller's own is not. Printed: "msgs". The probes, homings and prompts (D101, D102): a set of
// lines classified as FluidNC reads them.
//
// TimeLeft (§7, D91), for jobs of a minute or more: the job is "run" with each move taking 1.25 x
// its estimate + 0.05 s (no acceleration in the estimate), the feed override going to 150% at
// 40% of the moves in a second run; a report every 0.25 s. Printed: the error of the time left
// shown against the true one at 25, 50, 75 and 90% of the run ("left err"), the largest move of
// the value in one report beyond its countdown ("jump"), and the second run's errors at 75 and
// 90%. TRACE=<part of a name> prints that job's first run every 30 s, then every 5 minutes.
#include "gcode/analysis.h"
#include "services/job_notes.h"
#include "services/run_plan.h"
#include "services/run_tracker.h"
#include "services/segment_list.h"
#include "services/time_left.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

struct Exact {
    gcode::Segment s;
    double unit;
    bool arcOk;
    double sweep;
    uint32_t n;  // the line's N word, read from the text; 0 if none
};

// An N word at the start of the line, as the post-processors write it
uint32_t lineN(const gcode::Line &l) {
    size_t i = 0;
    while (i < l.length && (l.data[i] == ' ' || l.data[i] == '\t')) i++;
    if (i >= l.length || (l.data[i] != 'N' && l.data[i] != 'n')) return 0;
    uint32_t v = 0;
    bool digits = false;
    for (i++; i < l.length && l.data[i] >= '0' && l.data[i] <= '9'; i++) {
        v = v * 10 + (uint32_t)(l.data[i] - '0');
        digits = true;
    }
    return digits ? v : 0;
}

// Records the library's segments, and passes everything on to the packer
class Tee : public gcode::AnalysisSink {
public:
    Tee(SegmentList::Packer &p, JobNotes &n) : packer(p), notes(n) {}
    void segment(const gcode::Segment &s) override {
        last = s;
        have = true;
        packer.segment(s);
    }
    void line(const gcode::Line &l, const gcode::Token *t, uint16_t n, const gcode::LineState &st) override {
        if (have && last.line == l.index) {
            exact.push_back({last, st.units == gcode::UnitsMode::G20 ? 25.4 : 1.0,
                             st.arc.state == gcode::ArcState::Ok, st.arc.geometry.sweep, lineN(l)});
        }
        have = false;
        packer.line(l, t, n, st);
        notes.scanLine(l.index, l.data, l.length);
    }
    std::vector<Exact> exact;

private:
    SegmentList::Packer &packer;
    JobNotes &notes;
    gcode::Segment last = {};
    bool have = false;
};

int failures = 0;

void fail(const char *name, const char *what, uint32_t i, double got, double want) {
    if (failures < 40) printf("  FAIL %s: move %u: %s %.4f, expected %.4f\n", name, i, what, got, want);
    failures++;
}

bool same(const SegmentList::Move &a, const SegmentList::Move &b) {
    return a.kind == b.kind && a.line == b.line && a.n == b.n && a.x1 == b.x1 && a.y1 == b.y1 && a.z1 == b.z1 && a.x0 == b.x0 &&
           a.y0 == b.y0 && a.feed == b.feed && a.cx == b.cx && a.cy == b.cy;
}

// The messages as FluidNC may send them, and one of its own
void checkNotes(const char *name, const JobNotes &notes) {
    for (uint32_t i = 0; i < notes.messages(); i++) {
        if (i && notes.messageLine(i) < notes.messageLine(i - 1)) fail(name, "message order", i, notes.messageLine(i), 0);
        const char *text = notes.messageText(i);
        if (!notes.isFileMessage(text)) fail(name, "message not recognised", i, 0, 0);
        std::string packed;
        for (const char *c = text; *c; c++) {
            if (*c != ' ' && *c != '\t') packed += *c;
        }
        if (!notes.isFileMessage(packed.c_str())) fail(name, "message without spaces not recognised", i, 0, 0);
        if (packed.size() > 1 && !notes.isFileMessage(packed.c_str() + 1)) fail(name, "shifted message not recognised", i, 0, 0);
        if (strcmp(notes.messageAt(notes.messageLine(i)), text) != 0 &&
            (i + 1 >= notes.messages() || notes.messageLine(i + 1) != notes.messageLine(i))) {
            fail(name, "messageAt", i, notes.messageLine(i), 0);
        }
    }
    if (notes.isFileMessage("Report/Interval=250")) fail(name, "controller message taken as the file's", 0, 0, 0);
}

// One run of the time model; `errors` at 25, 50, 75 and 90% of it, in percent of the true time
// left; returns the largest move in one report beyond the countdown
double runTimeModel(const char *name, const SegmentList &list, const std::vector<SegmentList::Move> &moves,
                    const SegmentList::Estimate &est, const SegmentList::Rates &rates, bool override,
                    double errors[4]) {
    // Each move's actual time
    const size_t overrideAt = override ? moves.size() * 4 / 10 : moves.size();
    std::vector<double> actual(moves.size());
    double total = 0;
    for (size_t k = 0; k < moves.size(); k++) {
        bool inc = false;
        double s = SegmentList::moveSeconds(moves[k], rates, inc);
        double feed = SegmentList::feedPart(moves[k], s);
        double scaled = k >= overrideAt ? feed / 1.5 : feed;
        actual[k] = (s - feed + scaled) * 1.25 + 0.05;
        total += actual[k];
    }
    const char *trace = getenv("TRACE");
    bool tracing = trace && strstr(name, trace) && !override;
    TimeLeft tl;
    double t = 0, moveEnd = actual.empty() ? 0 : actual[0], prevShown = -1, lastFs = 1.0, nextTrace = 0;
    size_t k = 0, lastK = (size_t)-1;
    const double marks[4] = {0.25, 0.5, 0.75, 0.9};
    for (int i = 0; i < 4; i++) errors[i] = NAN;
    double jump = 0;
    for (; k < moves.size(); t += 0.25) {
        while (k < moves.size() && t >= moveEnd) {
            k++;
            if (k < moves.size()) moveEnd += actual[k];
        }
        if (k >= moves.size()) break;
        double fs = k >= overrideAt ? 1.5 : 1.0;
        if (k != lastK || fs != lastFs) {
            tl.match(t, RunPlan::at(list, nullptr, moves[k].record, rates, est.seconds, est.feedSeconds), fs, 1.0);
            lastK = k;
            lastFs = fs;
        }
        tl.tick(t);
        if (prevShown >= 0) jump = fmax(jump, fabs(tl.seconds() - (prevShown - 0.25)));
        prevShown = tl.seconds();
        double trueLeft = total - t;
        for (int i = 0; i < 4; i++) {
            if (isnan(errors[i]) && t >= marks[i] * total) errors[i] = (tl.seconds() - trueLeft) / trueLeft * 100;
        }
        if (tracing && t >= nextTrace) {
            printf("    t %6.0f s  shown %6.0f s  true %6.0f s  factor %.2f\n", t, tl.seconds(), trueLeft, tl.factor());
            nextTrace += t < 600 ? 30 : 300;
        }
    }
    return jump;
}

void timeModel(const char *name, const SegmentList &list, const std::vector<SegmentList::Move> &moves,
               const SegmentList::Estimate &est, const SegmentList::Rates &rates, char *out, size_t outLen) {
    double e[4], o[4];
    double jump = runTimeModel(name, list, moves, est, rates, false, e);
    runTimeModel(name, list, moves, est, rates, true, o);
    snprintf(out, outLen, "  left err %+.0f%% %+.0f%% %+.0f%% %+.0f%%, jump %.1f s; 150%% at 40%%: %+.0f%% %+.0f%%", e[0],
             e[1], e[2], e[3], jump, o[2], o[3]);
}

void check(const char *name, const std::string &text, size_t listBytes, bool quiet) {
    static std::vector<char> arena(8 << 20);
    std::vector<char> mem(listBytes);
    SegmentList list;
    list.attach(mem.data(), mem.size());
    SegmentList::Packer packer(list);
    static std::vector<char> notesMem(16 * 1024);
    JobNotes notes;
    notes.attach(notesMem.data(), notesMem.size());
    Tee tee(packer, notes);
    gcode::MemoryLineReader reader(text.data(), text.size());
    gcode::AnalysisOptions options;
    options.maxLineLength = 4096;
    gcode::Analysis a;
    gcode::Status s = gcode::analyse(reader, options, gcode::Arena{arena.data(), arena.size()}, &tee, a);
    if (s != gcode::Status::Ok) {
        if (!quiet) printf("%-48s status %d, skipped\n", name, (int)s);
        return;
    }

    int before = failures;
    double worstXY = 0, worstZ = 0, worstArc = 0, worstSweep = 0;
    SegmentList::Reader r(list);
    SegmentList::Move m;
    uint32_t i = 0;
    std::vector<SegmentList::Move> moves;
    for (; r.next(m); i++) {
        moves.push_back(m);
        if (i >= tee.exact.size()) {
            fail(name, "extra move", i, 0, 0);
            break;
        }
        const Exact &e = tee.exact[i];
        const gcode::Segment &g = e.s;
        if (m.line != g.line) fail(name, "line", i, m.line, g.line);
        if (list.nUsable() && e.n && m.n != e.n) fail(name, "N word", i, m.n, e.n);
        if (!list.nUsable() && m.n) fail(name, "N word without usable N", i, m.n, 0);
        double ex = g.x1 * e.unit, ey = g.y1 * e.unit, ez = g.z1 * e.unit;
        double dxy = fmax(fabs(m.x1 - ex), fabs(m.y1 - ey));
        worstXY = fmax(worstXY, dxy);
        if (dxy > list.xyStep() * 1.01) fail(name, "end XY error", i, dxy, list.xyStep());
        double dz = fabs(m.z1 - ez);
        worstZ = fmax(worstZ, dz);
        if (dz > list.zStep() * 1.01) fail(name, "end Z error", i, dz, list.zStep());
        bool libArc = (g.kind == gcode::SegmentKind::ArcCW || g.kind == gcode::SegmentKind::ArcCCW) && e.arcOk &&
                      !isnan(g.cx);
        if (g.kind == gcode::SegmentKind::Rapid && m.kind != SegmentList::RAPID) fail(name, "kind", i, m.kind, 0);
        if (libArc && (m.kind == SegmentList::ARC_CW || m.kind == SegmentList::ARC_CCW)) {
            if ((m.kind == SegmentList::ARC_CCW) != (g.kind == gcode::SegmentKind::ArcCCW)) fail(name, "arc direction", i, m.kind, (int)g.kind);
            // The arc's middle point, which is what the drawing shows: a centre far from a short
            // chord moves a lot with the rounded ends while the arc itself stays put
            double r0 = hypot(g.x0 - g.cx, g.y0 - g.cy) * e.unit;
            double ga = atan2(g.y0 - g.cy, g.x0 - g.cx) + e.sweep / 2;
            double gx = g.cx * e.unit + r0 * cos(ga), gy = g.cy * e.unit + r0 * sin(ga);
            double ma = atan2(m.y0 - m.cy, m.x0 - m.cx) + m.sweep / 2;
            double mx = m.cx + m.r * cos(ma), my = m.cy + m.r * sin(ma);
            double dc = hypot(mx - gx, my - gy);
            // An arc only a few steps across is off by about its own size
            double allowed = r0 > 4 * list.xyStep() ? 3 * list.xyStep() : 2 * r0 + 3 * list.xyStep();
            worstArc = fmax(worstArc, dc);
            // Beyond the jobs the list is for (10 m: a step under 1 mm), only XY and Z are checked
            if (dc > allowed && list.xyStep() < 1) fail(name, "arc middle error", i, dc, allowed);
            double ds = fabs(m.sweep - e.sweep);
            worstSweep = fmax(worstSweep, ds);
            if (ds > 0.2 && r0 > 20 * list.xyStep()) fail(name, "arc sweep", i, m.sweep, e.sweep);
        } else if (libArc && m.kind == SegmentList::FEED) {
            // Too small for a step, or not placeable: drawn straight. Only for tiny arcs
            double r0 = hypot(g.x0 - g.cx, g.y0 - g.cy) * e.unit;
            if (r0 > 2 * list.xyStep()) fail(name, "arc stored straight", i, r0, list.xyStep());
        }
    }
    if (!list.truncated() && i != tee.exact.size()) fail(name, "move count", i, i, (double)tee.exact.size());

    // A reader from each checkpoint reads what the full reader read from there
    for (uint32_t c = 0; c < list.checkpoints(); c++) {
        SegmentList::Reader rc(list, c);
        size_t k = 0;
        while (k < moves.size() && moves[k].record < c * SegmentList::CHECKPOINT_EVERY) k++;
        for (int n = 0; n < 300 && rc.next(m); n++, k++) {
            if (k >= moves.size() || !same(m, moves[k])) {
                fail(name, "checkpoint reader", (uint32_t)k, c, 0);
                break;
            }
        }
    }

    // Following the tool along the moves
    RunTracker tracker;
    uint32_t last = 0, onMove = 0;
    for (size_t k = 0; k < moves.size(); k++) {
        const SegmentList::Move &mv = moves[k];
        float x, y;
        if (mv.kind == SegmentList::ARC_CW || mv.kind == SegmentList::ARC_CCW) {
            float a = atan2f(mv.y0 - mv.cy, mv.x0 - mv.cx) + mv.sweep * 0.5f;
            x = mv.cx + mv.r * cosf(a);
            y = mv.cy + mv.r * sinf(a);
        } else {
            x = (mv.x0 + mv.x1) * 0.5f;
            y = (mv.y0 + mv.y1) * 0.5f;
        }
        float z = (mv.z0 + mv.z1) * 0.5f;
        tracker.update(list, x, y, z, RunTracker::READ_ALL);
        if (!tracker.matched()) {
            fail(name, "tracker: no match", (uint32_t)k, 0, 0);
            break;
        }
        if (tracker.record() < last) fail(name, "tracker: went back", (uint32_t)k, tracker.record(), last);
        if (tracker.record() > mv.record) fail(name, "tracker: ahead of the tool", (uint32_t)k, tracker.record(), mv.record);
        last = tracker.record();
        if (tracker.record() == mv.record) onMove++;
    }
    double follow = moves.empty() ? 100.0 : 100.0 * onMove / moves.size();

    // Following by `Ln:`: the very move, whatever the position
    char byLn[48] = "";
    if (list.nUsable()) {
        RunTracker t;
        uint32_t checked = 0;
        for (size_t k = 0; k < moves.size(); k++) {
            const SegmentList::Move &mv = moves[k];
            if (!mv.n) continue;
            t.update(list, 1e6f, 1e6f, 1e6f, RunTracker::READ_ALL, mv.n);
            if (!t.matched() || !t.byLineNumber() || t.record() != mv.record || t.line() != mv.line) {
                fail(name, "Ln match", (uint32_t)k, t.matched() ? t.record() : -1, mv.record);
                break;
            }
            checked++;
        }
        // A number no move has (a jog's) keeps the match
        uint32_t before = t.record();
        t.update(list, 1e6f, 1e6f, 1e6f, RunTracker::READ_ALL, 4000000000u);
        if (checked && t.record() != before) fail(name, "Ln unknown moved the match", 0, t.record(), before);
        snprintf(byLn, sizeof(byLn), "  Ln %u", checked);
    }

    SegmentList::Rates rates = {8000, 8000, 3000};
    SegmentList::Estimate est = list.estimate(rates);
    checkNotes(name, notes);
    char timing[160] = "";
    if (!list.truncated() && est.seconds >= 60) timeModel(name, list, moves, est, rates, timing, sizeof(timing));
    if (!quiet || failures != before) {
        printf("%-48s %6u moves %6u records %s step %.2f/%.1f mm  err xy %.3f z %.3f arc %.3f mm  est %.0f s%s%s  follow %.1f%%%s  msgs %u%s\n",
               name, list.moves(), list.records(), list.truncated() ? "TRUNCATED" : "", list.xyStep(), list.zStep(),
               worstXY, worstZ, worstArc, est.seconds, est.incomplete ? "+" : "", list.nUsable() ? "  N" : "", follow,
               byLn, notes.messages(), timing);
    }
}

std::string readFile(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "";
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

}  // namespace

// JobNotes::classify (D101, D102): what counts as a probe, a homing or a prompt
void checkClassify() {
    struct Case {
        const char *line;
        JobNotes::Work want;
    } cases[] = {
        {"G38.2 Z-110 F200 P0.5 (probe down set thickness)", JobNotes::PROBE},
        {"g38.3 z-5", JobNotes::PROBE},
        {"G 38.2Z-1", JobNotes::PROBE},
        {"G1 X38 G38.4 Z-2", JobNotes::PROBE},
        {"$HZ", JobNotes::HOME},
        {"  $H", JobNotes::HOME},
        {"$h", JobNotes::HOME},
        {"$X", JobNotes::NONE},
        {"$$", JobNotes::NONE},
        {"M0 (MSG Insert T3 1/4in flat endmill, attach probe)", JobNotes::PROMPT},
        {"M00", JobNotes::PROMPT},
        {"m0", JobNotes::PROMPT},
        {"M03 S12000", JobNotes::NONE},
        {"M3 S12000", JobNotes::NONE},
        {"M1", JobNotes::NONE},
        {"M30", JobNotes::NONE},
        {"(M0 in a comment) G1 X1", JobNotes::NONE},
        {"G1 X1 ; M0 after a semicolon", JobNotes::NONE},
        {"(MSG G38.2 named in a message)", JobNotes::NONE},
        {"G0 X38.2 Y-38", JobNotes::NONE},
        {"G4 P0 ; wait for motion to finish before $ command", JobNotes::NONE},
        {"(Begin toolchange)", JobNotes::NONE},
        {"", JobNotes::NONE},
    };
    for (const Case &c : cases) {
        JobNotes::Work got = JobNotes::classify(c.line, (uint32_t)strlen(c.line));
        if (got != c.want) {
            failures++;
            printf("  FAIL classify \"%s\": %d, expected %d\n", c.line, (int)got, (int)c.want);
        }
    }
    // The allowances by line
    static char mem[16 * 1024];
    JobNotes notes;
    notes.attach(mem, sizeof(mem));
    const char *lines[] = {"G0 X10 Y10", "M0 (MSG a)", "G38.2 Z-110 F200", "G1 Z15", "M0 (MSG b)", "G1 X20", "$HZ",
                           "G38.2 Z-110", "M0", "G1 X30"};
    for (uint32_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) notes.scanLine(i, lines[i], (uint32_t)strlen(lines[i]));
    if (notes.probes() != 2 || notes.homings() != 1 || notes.prompts() != 3) {
        failures++;
        printf("  FAIL counts: %u probes %u homings %u prompts\n", notes.probes(), notes.homings(), notes.prompts());
    }
    if (notes.workSeconds() != 2 * JobNotes::PROBE_S + JobNotes::HOME_S || notes.workSecondsAfter(0) != notes.workSeconds() ||
        notes.workSecondsAfter(2) != JobNotes::PROBE_S + JobNotes::HOME_S || notes.workSecondsAfter(7) != 0) {
        failures++;
        printf("  FAIL workSecondsAfter\n");
    }
    if (!notes.workBetween(0, 5) || notes.workBetween(2, 5) || !notes.workBetween(5, 9) || notes.workBetween(7, 9)) {
        failures++;
        printf("  FAIL workBetween\n");
    }
    // The messages still fit beside the work lines
    if (notes.messages() != 2 || strcmp(notes.messageAt(3), "a") != 0) {
        failures++;
        printf("  FAIL messages beside the work lines\n");
    }
}

int main(int argc, char **argv) {
    int files = 0;
    checkClassify();
    bool quiet = getenv("QUIET") != nullptr;
    for (int i = 1; i < argc; i++) {
        std::string text = readFile(argv[i]);
        if (text.empty()) continue;
        const char *name = argv[i];
        size_t len = strlen(name);
        const char *shown = len > 48 ? name + len - 48 : name;
        check(shown, text, 640 * 1024, quiet);
        files++;
    }

    // A job over 327 mm doubles the step; a list too small truncates
    std::string big = "G21 G90\nG0 Z5\nG0 X0 Y0\nG1 Z-1 F500\n";
    for (int k = 0; k <= 30; k++) {
        char line[96];
        snprintf(line, sizeof(line), "G1 X%d Y%d F1000\nG2 X%d Y%d I0 J50\n", k * 50, (k % 2) * 800, k * 50,
                 (k % 2) * 800 + 100);
        big += line;
    }
    big += "G0 Z5\nG92 X0 Y0\nG0 X10 Y10\nG4 P2.5\nG1 X20 F600\nG0 Z40\n";
    check("synthetic 1500 x 900 mm", big, 640 * 1024, false);
    check("synthetic, 400-byte list", big, 400, false);

    printf("%d files, %d failures\n", files, failures);
    return failures ? 1 : 0;
}
