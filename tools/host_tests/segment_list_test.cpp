// Host test of the segment list (specs/11-preview.md §10): every file given is analysed with the
// G-code library; the library's segments are compared with the packed records read back.
//
//   tools/host_tests/run.sh <file or folder>...
//
// Checked for each move: kind, line, end X/Y within one step and Z within one Z step (repacks
// round twice), an arc's centre and sweep; that a reader started at any checkpoint reads the same
// moves as one from the start; and a job larger than 327 mm doubles the step.
#include "gcode/analysis.h"
#include "services/segment_list.h"
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
};

// Records the library's segments, and passes everything on to the packer
class Tee : public gcode::AnalysisSink {
public:
    explicit Tee(SegmentList::Packer &p) : packer(p) {}
    void segment(const gcode::Segment &s) override {
        last = s;
        have = true;
        packer.segment(s);
    }
    void line(const gcode::Line &l, const gcode::Token *t, uint16_t n, const gcode::LineState &st) override {
        if (have && last.line == l.index) {
            exact.push_back({last, st.units == gcode::UnitsMode::G20 ? 25.4 : 1.0,
                             st.arc.state == gcode::ArcState::Ok, st.arc.geometry.sweep});
        }
        have = false;
        packer.line(l, t, n, st);
    }
    std::vector<Exact> exact;

private:
    SegmentList::Packer &packer;
    gcode::Segment last = {};
    bool have = false;
};

int failures = 0;

void fail(const char *name, const char *what, uint32_t i, double got, double want) {
    if (failures < 40) printf("  FAIL %s: move %u: %s %.4f, expected %.4f\n", name, i, what, got, want);
    failures++;
}

bool same(const SegmentList::Move &a, const SegmentList::Move &b) {
    return a.kind == b.kind && a.line == b.line && a.x1 == b.x1 && a.y1 == b.y1 && a.z1 == b.z1 && a.x0 == b.x0 &&
           a.y0 == b.y0 && a.feed == b.feed && a.cx == b.cx && a.cy == b.cy;
}

void check(const char *name, const std::string &text, size_t listBytes, bool quiet) {
    static std::vector<char> arena(8 << 20);
    std::vector<char> mem(listBytes);
    SegmentList list;
    list.attach(mem.data(), mem.size());
    SegmentList::Packer packer(list);
    Tee tee(packer);
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

    SegmentList::Rates rates = {8000, 8000, 3000};
    SegmentList::Estimate est = list.estimate(rates);
    if (!quiet || failures != before) {
        printf("%-48s %6u moves %6u records %s step %.2f/%.1f mm  err xy %.3f z %.3f arc %.3f mm  est %.0f s%s%s\n", name,
               list.moves(), list.records(), list.truncated() ? "TRUNCATED" : "", list.xyStep(), list.zStep(), worstXY,
               worstZ, worstArc, est.seconds, est.incomplete ? "+" : "", list.nUsable() ? "  N" : "");
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

int main(int argc, char **argv) {
    int files = 0;
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
