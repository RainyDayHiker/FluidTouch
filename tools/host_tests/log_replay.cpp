// A job's time estimate, and Run's time left replayed against a position log (Settings > Tools),
// with the display's own SegmentList, RunTracker and TimeLeft. Plain host build; see replay.sh.
//
//   log_replay <file.gcode> [position-log.csv] [x,y,z axis rates mm/min]
//
// Prints the estimate (D80, specs/11-preview.md §5), the estimate per part of the file (each part
// runs from one `(MSG ...)` comment to the next: the operations, for files that name them), and,
// with a log, every 3 minutes of running time the time left the display would show (§7, D91),
// the time really left and the correction factor. Running time is the log's Run and Home rows
// (the waits at a prompt or a pause are Hold); the overrides are taken as 100% (the log has none).
// The real cut's time per part is in the log: the rows from one message to the next.
#include "gcode/analysis.h"
#include "gcode/io.h"
#include "services/job_notes.h"
#include "services/run_tracker.h"
#include "services/segment_list.h"
#include "services/time_left.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string readFile(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "";
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// As RunFollow's planAt: planned seconds before the record, and after it
TimeLeft::Plan planAt(const SegmentList &list, uint32_t record, const SegmentList::Rates &rates,
                      const SegmentList::Estimate &est) {
    TimeLeft::Plan p = {0, 0, 0, 0, est.feedSeconds, est.seconds - est.feedSeconds};
    if (!list.checkpoints()) return p;
    uint32_t cp = record / SegmentList::CHECKPOINT_EVERY;
    if (cp >= list.checkpoints()) cp = list.checkpoints() - 1;
    double total = list.checkpoint(cp).seconds, feed = list.checkpoint(cp).feedSeconds;
    SegmentList::Reader reader(list, cp);
    SegmentList::Move m;
    bool incomplete = false, more;
    while ((more = reader.next(m)) && m.record < record) {
        double s = SegmentList::moveSeconds(m, rates, incomplete);
        total += s;
        feed += SegmentList::feedPart(m, s);
    }
    p.feedDone = p.feedAfter = feed;
    p.otherDone = p.otherAfter = total - feed;
    if (more && m.record == record) {
        double s = SegmentList::moveSeconds(m, rates, incomplete);
        p.feedAfter += SegmentList::feedPart(m, s);
        p.otherAfter += s - SegmentList::feedPart(m, s);
    }
    return p;
}

// As RunFollow's opEndAt: the plan at the end of operation `op`'s last move, from `record` on
bool opEndAt(const SegmentList &list, const JobNotes &notes, const SegmentList::Rates &rates, int op, uint32_t record,
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

// The packer, and the messages and lines for JobNotes, as PreviewService's sink
class Sink : public gcode::AnalysisSink {
public:
    Sink(SegmentList::Packer &p, JobNotes &n) : packer(p), notes(n) {}
    void segment(const gcode::Segment &s) override { packer.segment(s); }
    void line(const gcode::Line &l, const gcode::Token *t, uint16_t n, const gcode::LineState &st) override {
        packer.line(l, t, n, st);
        notes.scanLine(l.index, l.data, l.length);
    }

private:
    SegmentList::Packer &packer;
    JobNotes &notes;
};

struct Row {
    long ms;
    bool running;
    double percent;  // -1 when the log has none (not printing from SD)
    float x, y, z;
};

// ms,state,percent,wx,wy,wz,feed,message
bool readLog(const char *path, std::vector<Row> &rows) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[1024];
    if (!fgets(line, sizeof line, f)) {
        fclose(f);
        return false;
    }
    while (fgets(line, sizeof line, f)) {
        char *field[6];
        char *p = line;
        int n = 0;
        for (; n < 6 && p; n++) {
            field[n] = p;
            p = strchr(p, ',');
            if (p) *p++ = '\0';
        }
        if (n < 6) continue;
        Row r;
        r.ms = strtol(field[0], nullptr, 10);
        r.running = !strcmp(field[1], "Run") || !strcmp(field[1], "Home");
        r.percent = field[2][0] ? atof(field[2]) : -1;
        r.x = strtof(field[3], nullptr);
        r.y = strtof(field[4], nullptr);
        r.z = strtof(field[5], nullptr);
        rows.push_back(r);
    }
    fclose(f);
    return true;
}

void replay(const SegmentList &list, const JobNotes &notes, const SegmentList::Rates &rates,
            const SegmentList::Estimate &est, const std::vector<Row> &rows, size_t fileSize) {
    std::vector<double> run(rows.size(), 0);
    for (size_t i = 1; i < rows.size(); i++)
        run[i] = run[i - 1] + (rows[i - 1].running ? (rows[i].ms - rows[i - 1].ms) / 1000.0 : 0);
    double total = run.empty() ? 0 : run.back();
    printf("\nRunning %.1f min of %.1f min in the log\n", total / 60, rows.empty() ? 0 : rows.back().ms / 60000.0);
    RunTracker tracker;
    TimeLeft left;
    double nextPrint = 0, worst = 0, worstAt = 0;
    // The operation's time left (D100): where each op's matches start and end in running time,
    // so its true time left is known afterwards
    struct OpSample {
        double run, shown;
        int op;
    };
    std::vector<OpSample> opSamples;
    std::vector<double> opLastRun(JobNotes::MAX_OPS + 1, -1);
    int op = 0, opFor = 0;
    bool opFound = false;
    double opFeed = 0, opOther = 0;
    std::vector<std::string> table;
    for (size_t i = 0; i < rows.size(); i++) {
        left.tick(run[i]);
        if (rows[i].percent < 0) continue;
        // As RunFollow: what FluidNC has read, from the SD: percent, with a little slack
        uint64_t read = (uint64_t)(rows[i].percent / 100.0 * (double)fileSize) + 1024;
        bool moved = tracker.update(list, rows[i].x, rows[i].y, rows[i].z, read);
        if (!tracker.matched()) continue;
        if (moved || !left.known()) left.match(run[i], planAt(list, tracker.record(), rates, est), 1, 1);
        int at = notes.opAt(tracker.line());
        if (at) {
            op = at;
            if (opFor != op) {
                opFound = opEndAt(list, notes, rates, op, tracker.record(), opFeed, opOther);
                opFor = op;
            }
            opLastRun[op] = run[i];
            if (opFound) opSamples.push_back({run[i], fmax(1, left.leftUntil(opFeed, opOther, run[i])), op});
        }
        double err = left.seconds() - (total - run[i]);
        if (run[i] >= 15 * 60 && fabs(err) > fabs(worst)) {
            worst = err;
            worstAt = run[i];
        }
        if (run[i] >= nextPrint) {
            char t[160];
            snprintf(t, sizeof t, "%8.1f %10.1f %8.1f %7.2f", run[i] / 60, (total - run[i]) / 60, left.seconds() / 60,
                     left.factor());
            table.push_back(t);
            nextPrint += 180;
        }
    }
    // The op columns, now that each op's end in the log is known: its last matched row
    printf("%8s %10s %8s %7s %4s %9s %8s   (minutes)\n", "running", "true left", "shown", "factor", "op", "op left",
           "shown");
    size_t k = 0;
    double next = 0;
    for (const std::string &t : table) {
        while (k < opSamples.size() && opSamples[k].run < next) k++;
        if (k < opSamples.size() && opSamples[k].run < next + 180) {
            const OpSample &o = opSamples[k];
            printf("%s %4d %9.1f %8.1f\n", t.c_str(), o.op, (opLastRun[o.op] - o.run) / 60, o.shown / 60);
        } else {
            printf("%s %4s\n", t.c_str(), "-");
        }
        next += 180;
    }
    printf("From 15 min of running on, the furthest off: %+.1f min at %.1f min\n", worst / 60, worstAt / 60);
    // Per op: how far the op's time left was off, 1 minute after it started and at its middle
    printf("\n%4s %8s %13s %13s   (minutes; true is to the op's last matched report)\n", "op", "length",
           "off at 1 min", "off at half");
    for (int o = 1; o <= JobNotes::MAX_OPS; o++) {
        if (opLastRun[o] < 0) continue;
        size_t a = 0;
        while (a < opSamples.size() && opSamples[a].op != o) a++;
        if (a == opSamples.size()) continue;
        double start = opSamples[a].run, len = opLastRun[o] - start;
        double offAt[2] = {NAN, NAN};
        double when[2] = {start + 60, start + len / 2};
        for (int w = 0; w < 2; w++) {
            for (size_t b = a; b < opSamples.size() && opSamples[b].op == o; b++) {
                if (opSamples[b].run >= when[w]) {
                    offAt[w] = (opSamples[b].shown - (opLastRun[o] - opSamples[b].run)) / 60;
                    break;
                }
            }
        }
        printf("%4d %8.1f %+13.1f %+13.1f\n", o, len / 60, offAt[0], offAt[1]);
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: log_replay <file.gcode> [position-log.csv] [x,y,z axis rates mm/min]\n");
        return 2;
    }
    std::string text = readFile(argv[1]);
    if (text.empty()) {
        fprintf(stderr, "can't read %s\n", argv[1]);
        return 1;
    }
    // The parts: each `(MSG ...)` comment at the start of a line (0-based line numbers)
    std::vector<std::pair<uint32_t, std::string>> parts;
    uint32_t ln = 0;
    for (size_t p = 0; p < text.size(); ln++) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        size_t at = text.find("(MSG ", p);
        if (at < e) {
            std::string label = text.substr(at + 5, e - at - 5);
            while (!label.empty() && (label.back() == '\r' || label.back() == ')')) label.pop_back();
            parts.push_back({ln, label.substr(0, 40)});
        }
        p = e + 1;
    }

    static std::vector<char> arena(8 << 20);
    std::vector<char> mem(640 * 1024);  // as the device's list (PreviewService)
    SegmentList list;
    list.attach(mem.data(), mem.size());
    SegmentList::Packer packer(list);
    std::vector<char> notesMem(16 * 1024);  // as the device's (11 §6)
    JobNotes notes;
    notes.attach(notesMem.data(), notesMem.size());
    Sink sink(packer, notes);
    gcode::MemoryLineReader reader(text.data(), text.size());
    gcode::AnalysisOptions options;
    options.maxLineLength = 4096;
    gcode::Analysis analysis;
    gcode::Status s = gcode::analyse(reader, options, gcode::Arena{arena.data(), arena.size()}, &sink, analysis);
    if (s != gcode::Status::Ok) {
        fprintf(stderr, "analysis status %d\n", (int)s);
        return 1;
    }
    // The enabled operations, numbered as PreviewService numbers them (D92)
    int ops = 0;
    if (analysis.structure.kind == gcode::StructureKind::Ok) {
        for (uint32_t i = 0; i < analysis.structure.unitCount; i++) {
            const gcode::Unit &u = analysis.structure.units[i];
            if (!u.enabled) continue;
            ops++;
            notes.addOp(u.body.start, u.body.end, (uint16_t)ops);
            for (uint32_t f = 0; f < u.fixtureCount; f++) notes.addOp(u.fixtures[f].start, u.fixtures[f].end, (uint16_t)ops);
        }
        notes.finishOps();
    }

    SegmentList::Rates rates = {9000, 9000, 900};  // Jeffrey's LowRider
    if (argc > 3 && sscanf(argv[3], "%f,%f,%f", &rates.x, &rates.y, &rates.z) != 3) {
        fprintf(stderr, "rates as x,y,z\n");
        return 2;
    }
    SegmentList::Estimate est = list.estimate(rates);
    printf("%s: %u moves%s, rates %.0f %.0f %.0f mm/min\n", argv[1], list.moves(), list.truncated() ? " (TRUNCATED)" : "",
           rates.x, rates.y, rates.z);
    printf("Estimate %.1f min%s, of which feeds and arcs %.1f min\n\n", est.seconds / 60, est.incomplete ? "+" : "",
           est.feedSeconds / 60);

    std::vector<double> per(parts.size() + 1, 0), perRapid(parts.size() + 1, 0);
    SegmentList::Reader r(list);
    SegmentList::Move m;
    while (r.next(m)) {
        bool incomplete = false;
        double t = SegmentList::moveSeconds(m, rates, incomplete);
        size_t k = 0;
        while (k < parts.size() && parts[k].first <= m.line) k++;
        per[k] += t;
        if (m.kind == SegmentList::RAPID) perRapid[k] += t;
    }
    printf("%-40s %8s %8s\n", "from the message", "est min", "rapids");
    for (size_t k = 0; k <= parts.size(); k++) {
        if (k == 0 && per[0] == 0) continue;
        printf("%-40s %8.2f %8.2f\n", k == 0 ? "(before the first message)" : parts[k - 1].second.c_str(), per[k] / 60,
               perRapid[k] / 60);
    }

    if (argc > 2) {
        std::vector<Row> rows;
        if (!readLog(argv[2], rows)) {
            fprintf(stderr, "can't read %s\n", argv[2]);
            return 1;
        }
        if (list.truncated()) printf("\nThe list is truncated: the display shows the percent formula instead\n");
        else replay(list, notes, rates, est, rows, text.size());
    }
    return 0;
}
