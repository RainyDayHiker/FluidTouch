#include "services/preview_service.h"
#include "gcode/analysis.h"
#include "gcode/num.h"
#include "network/fluidnc_client.h"
#include "services/machine_service.h"
#include "services/sd_reader.h"
#include <Arduino.h>
#include <SD.h>
#include <errno.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <strings.h>

const char *const PreviewService::WORK_FOLDER = "/lrt/work";

namespace {

// The library's arena, held while a job is selected (D77)
constexpr size_t ARENA_SIZE = 512 * 1024;
// The segment list: the largest free block less some headroom (Run's canvas is allocated at
// boot, D89), at most LIST_MAX (about 80,000 records, almost 6 times the reference job, D30)
constexpr size_t LIST_MAX = 640 * 1024;
constexpr size_t LIST_MIN = 64 * 1024;
constexpr size_t RUN_RESERVE = 128 * 1024;
// The file's messages and operations (D92): the reference job's 93 messages take 4 KB
constexpr size_t NOTES_BYTES = 16 * 1024;
// Download buffer and timeouts
constexpr size_t FETCH_CHUNK = 16384;
constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
constexpr uint32_t DATA_TIMEOUT_MS = 10000;

struct Request {
    FileService::Card card;
    char path[FileService::PATH_LEN];
    char name[FileService::NAME_LEN];
    int32_t size;
    bool duringJob;  // Run asked for it during the cut (D72): no waiting for the end
    bool useCopy;    // the working folder's copy, no download (D94)
};

Request request = {};       // the chosen file
bool pendingStart = false;  // start() once the task is free
volatile bool taskRunning = false;
volatile bool cancelFlag = false;
volatile uint32_t generation = 0;  // load() moves it on; an older task's results are ignored

void *arena = nullptr;
void *listMemory = nullptr;
size_t listBytes = 0;
SegmentList list;
void *notesMemory = nullptr;
JobNotes notes;
uint32_t readyGen = 0;
uint32_t estimateRatesGen = 0;
uint32_t estimateGen = 0;

// The service's state; the task writes it only for its own generation
volatile PreviewService::State current = PreviewService::NONE;
PreviewService::Summary result = {};
// The reader of the analysis under way, for the bar between the library's checkpoints
SdReader *volatile activeReader = nullptr;

void setState(uint32_t gen, PreviewService::State s) {
    if (gen == generation) current = s;
}

// Written by the task, read by the UI
std::mutex lock;
char failText[160] = "";
volatile uint64_t doneBytes = 0;   // fetched, or read in this pass
volatile uint64_t totalBytes = 0;
volatile uint32_t passNumber = 1;

void copy(char *out, size_t outLen, const char *text) {
    strncpy(out, text, outLen - 1);
    out[outLen - 1] = '\0';
}

// "/sd/My Jobs/a b.gcode" -> "/sd/My%20Jobs/a%20b.gcode"
void encodePath(char *out, size_t outLen, const char *path) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)path; *p && n + 4 < outLen; p++) {
        unsigned char c = *p;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' ||
                     c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
}

bool ensureFolder(const char *path) { return SD.exists(path) || SD.mkdir(path); }

// The working folder, empty (02 §5)
bool clearWorkFolder() {
    if (!ensureFolder("/lrt") || !ensureFolder(PreviewService::WORK_FOLDER)) return false;
    File dir = SD.open(PreviewService::WORK_FOLDER);
    if (!dir || !dir.isDirectory()) return false;
    char names[16][FileService::NAME_LEN];
    int count;
    do {
        // Collected first: removing while iterating skips entries
        count = 0;
        dir.rewindDirectory();
        for (File f = dir.openNextFile(); f && count < 16; f = dir.openNextFile()) {
            if (!f.isDirectory()) {
                const char *slash = strrchr(f.name(), '/');
                copy(names[count++], FileService::NAME_LEN, slash ? slash + 1 : f.name());
            }
            f.close();
        }
        for (int i = 0; i < count; i++) {
            char full[FileService::PATH_LEN];
            FileService::join(full, sizeof(full), PreviewService::WORK_FOLDER, names[i]);
            SD.remove(full);
        }
    } while (count == 16);
    dir.close();
    return true;
}

// One header line, without its \r\n; false on a timeout or a closed connection
bool readHeaderLine(WiFiClient &client, char *out, size_t outLen) {
    size_t n = 0;
    uint32_t since = millis();
    while (true) {
        if (cancelFlag) return false;
        if (client.available()) {
            int c = client.read();
            if (c < 0) continue;
            since = millis();
            if (c == '\n') break;
            if (c != '\r' && n + 1 < outLen) out[n++] = (char)c;
        } else if (!client.connected()) {
            return false;
        } else if (millis() - since >= DATA_TIMEOUT_MS) {
            return false;
        } else {
            delay(2);
        }
    }
    out[n] = '\0';
    return true;
}

enum class Outcome { Ok, Failed, Cancelled };

Outcome fail(const char *text) {
    std::lock_guard<std::mutex> g(lock);
    copy(failText, sizeof(failText), text);
    return Outcome::Failed;
}

// The last fetch failed writing to the display SD (an I/O error: worth one more try, D86)
bool writeFailed = false;

// HTTP GET http://<controller>/sd/<path> into localPath (05 §2, S1)
Outcome fetch(const Request &req, const char *localPath, uint64_t &got) {
    got = 0;
    writeFailed = false;
    String host = FluidNCClient::getMachineIP();
    if (!FluidNCClient::isConnected() || host.length() == 0) return fail("The controller is offline.");
    if (!clearWorkFolder()) return fail("The display SD's working folder could not be made.");

    WiFiClient client;
    if (!client.connect(host.c_str(), 80, CONNECT_TIMEOUT_MS)) return fail("The controller didn't answer the download.");
    char url[FileService::PATH_LEN * 3];
    encodePath(url, sizeof(url), req.path);
    client.print(String("GET ") + url + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n");

    char line[256];
    if (!readHeaderLine(client, line, sizeof(line))) {
        client.stop();
        return cancelFlag ? Outcome::Cancelled : fail("The controller didn't answer the download.");
    }
    int code = 0;
    if (sscanf(line, "HTTP/%*s %d", &code) != 1 || code != 200) {
        client.stop();
        if (code == 404) return fail("The file is no longer on the controller's SD.");
        char text[160];
        snprintf(text, sizeof(text), "The controller refused the download (%s).", line);
        return fail(text);
    }
    int64_t length = -1;
    while (readHeaderLine(client, line, sizeof(line)) && line[0]) {
        if (strncasecmp(line, "Content-Length:", 15) == 0) length = atoll(line + 15);
    }
    if (cancelFlag) {
        client.stop();
        return Outcome::Cancelled;
    }
    totalBytes = length >= 0 ? (uint64_t)length : (req.size > 0 ? (uint64_t)req.size : 0);

    File out = SD.open(localPath, FILE_WRITE);
    if (!out) {
        client.stop();
        return fail("The download could not be written to the display SD.");
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(FETCH_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        out.close();
        client.stop();
        SD.remove(localPath);
        return fail("Not enough memory to download the job.");
    }
    uint32_t since = millis();
    Outcome outcome = Outcome::Ok;
    while (length < 0 || (int64_t)got < length) {
        if (cancelFlag) {
            outcome = Outcome::Cancelled;
            break;
        }
        int avail = client.available();
        if (avail > 0) {
            int n = client.read(buf, avail < (int)FETCH_CHUNK ? (size_t)avail : FETCH_CHUNK);
            if (n <= 0) continue;
            size_t wrote = out.write(buf, (size_t)n);
            if (wrote != (size_t)n) {
                int err = errno;
                Serial.printf("PreviewService: write failed at %llu bytes: %u of %d written, errno %d; "
                              "card %llu MB, used %llu MB\n",
                              (unsigned long long)got, (unsigned)wrote, n, err,
                              (unsigned long long)(SD.totalBytes() >> 20), (unsigned long long)(SD.usedBytes() >> 20));
                writeFailed = true;
                outcome = fail("Writing to the display SD failed.");
                break;
            }
            got += (uint64_t)n;
            doneBytes = got;
            since = millis();
            SdReader::yieldNow();
        } else if (!client.connected()) {
            break;  // the end, for a reply without a length
        } else if (millis() - since >= DATA_TIMEOUT_MS) {
            outcome = fail("The download stopped: no data from the controller for 10 s.");
            break;
        } else {
            delay(2);
        }
    }
    free(buf);
    out.close();
    client.stop();
    if (outcome == Outcome::Ok) {
        bool shortOfLength = length >= 0 && (int64_t)got != length;
        bool shortOfSize = req.size >= 0 && got != (uint64_t)req.size;
        if (shortOfLength || shortOfSize) {
            char text[160];
            snprintf(text, sizeof(text), "The download was short: %llu of %lld bytes.", (unsigned long long)got,
                     (long long)(shortOfSize ? req.size : length));
            outcome = fail(text);
        }
    }
    if (outcome != Outcome::Ok) SD.remove(localPath);
    return outcome;
}

const char *statusText(gcode::Status s) {
    switch (s) {
        case gcode::Status::ReadError: return "The display SD could not be read.";
        case gcode::Status::OutOfMemory: return "Not enough memory to analyse this job.";
        default: return "The job could not be analysed.";
    }
}

struct Work {
    Request req;
    uint32_t generation;
};

SegmentList::Rates currentRates() {
    return {MachineService::axisRate(0), MachineService::axisRate(1), MachineService::axisRate(2)};
}

// "T1 T2" from the structure's tool labels
void formatTools(char *out, size_t outLen, const gcode::Structure &s) {
    out[0] = '\0';
    size_t n = 0;
    for (uint32_t i = 0; i < s.labelCount && n + 12 < outLen; i++) {
        double t = s.labelTools[i];
        int w = t == (double)(long)t ? snprintf(out + n, outLen - n, "%sT%ld", n ? " " : "", (long)t)
                                     : snprintf(out + n, outLen - n, "%sT%g", n ? " " : "", t);
        if (w < 0) break;
        n += (size_t)w;
    }
}

// The packer, timed (the log splits analyse()'s time into reading, packing and the rest); it
// also collects the file's messages
class TimedPacker : public gcode::AnalysisSink {
public:
    explicit TimedPacker(SegmentList::Packer &p) : packer(p) {}
    void segment(const gcode::Segment &s) override {
        uint32_t t0 = micros();
        packer.segment(s);
        us += micros() - t0;
    }
    void line(const gcode::Line &l, const gcode::Token *t, uint16_t n, const gcode::LineState &st) override {
        uint32_t t0 = micros();
        packer.line(l, t, n, st);
        notes.scanLine(l.index, l.data, l.length);
        us += micros() - t0;
    }
    uint32_t us = 0;

private:
    SegmentList::Packer &packer;
};

// The info line's figures and the drawing's overlays, from the analysis and the list
void summarise(PreviewService::Summary &sum, const gcode::Analysis &a, uint64_t fileSize) {
    const double unit = a.units == gcode::Units::In ? 25.4 : 1.0;
    const int p = a.precision;
    auto mm = [&](double fixed) { return gcode::toNumber(fixed, p) * unit; };
    sum.hasJob = a.jobExtents.present;
    if (sum.hasJob) {
        sum.jobMinX = mm(a.jobExtents.minX);
        sum.jobMaxX = mm(a.jobExtents.maxX);
        sum.jobMinY = mm(a.jobExtents.minY);
        sum.jobMaxY = mm(a.jobExtents.maxY);
    }
    sum.hasStock = a.stockBox.present;
    if (sum.hasStock) {
        sum.stockMinX = mm(a.stockBox.minX);
        sum.stockMaxX = mm(a.stockBox.maxX);
        sum.stockMinY = mm(a.stockBox.minY);
        sum.stockMaxY = mm(a.stockBox.maxY);
    }
    sum.hasProbe = false;
    for (uint32_t i = 0; i < a.fixedMoveCount && !sum.hasProbe; i++) {
        if (a.fixedMoves[i].kind != gcode::FixedMoveKind::Probe) continue;
        for (int k = 0; k < list.fixedCount(); k++) {
            if (list.fixedLine(k) == a.fixedMoves[i].line) {
                sum.hasProbe = true;
                sum.probeX = list.fixedX(k);
                sum.probeY = list.fixedY(k);
                break;
            }
        }
    }
    sum.hasZ = list.hasZ();
    sum.zMin = list.zMin();
    sum.zMax = list.zMax();
    formatTools(sum.tools, sizeof(sum.tools), a.structure);
    sum.probes = notes.probes();
    sum.homings = notes.homings();
    sum.prompts = notes.prompts();
    sum.allowanceSeconds = notes.workSeconds() + notes.prompts() * JobNotes::PROMPT_S;
    sum.ops = -1;
    if (a.structure.kind == gcode::StructureKind::Ok) {
        sum.ops = 0;
        for (uint32_t i = 0; i < a.structure.unitCount; i++) {
            const gcode::Unit &u = a.structure.units[i];
            if (!u.enabled) continue;
            sum.ops++;
            // Its lines: the body and its fixtures (D92)
            notes.addOp(u.body.start, u.body.end, (uint16_t)sum.ops);
            for (uint32_t f = 0; f < u.fixtureCount; f++) {
                notes.addOp(u.fixtures[f].start, u.fixtures[f].end, (uint16_t)sum.ops);
            }
        }
        notes.finishOps();
    }
    sum.unitsAssumed = a.unitsAssumed;
    sum.unitsMixed = a.unitsMixed;
    sum.skipped = list.skippedXY();
    sum.shownPercent = 100;
    if (list.truncated() && fileSize) {
        uint64_t pct = list.truncatedOffset() * 100 / fileSize;
        sum.shownPercent = (uint8_t)(pct > 99 ? 99 : pct);
    }
    sum.moves = list.moves();
    SegmentList::Estimate e = list.estimate(currentRates());
    sum.estimateSeconds = e.seconds;
    sum.estimateFeedSeconds = e.feedSeconds;
    sum.estimateIncomplete = e.incomplete;
}

}  // namespace

// --- The worker task ---------------------------------------------------------------------

namespace {

void task(void *param) {
    Work *work = (Work *)param;
    const Request &req = work->req;
    uint32_t gen = work->generation;
    PreviewService::Summary sum = {};

    char localPath[FileService::PATH_LEN];
    Outcome outcome = Outcome::Ok;
    if (req.card == FileService::CARD_CONTROLLER && req.useCopy) {
        FileService::join(localPath, sizeof(localPath), PreviewService::WORK_FOLDER, req.name);
        Serial.printf("PreviewService: %s: the working folder's copy, no download\n", req.path);
    } else if (req.card == FileService::CARD_CONTROLLER) {
        FileService::join(localPath, sizeof(localPath), PreviewService::WORK_FOLDER, req.name);
        uint32_t t0 = millis();
        uint64_t got = 0;
        outcome = fetch(req, localPath, got);
        if (outcome == Outcome::Failed && writeFailed && !cancelFlag) {
            // One write error on the display SD is retried from the start (D86)
            Serial.printf("PreviewService: fetch %s: write failed at %llu bytes, trying again\n", req.path,
                          (unsigned long long)got);
            outcome = fetch(req, localPath, got);
        }
        sum.fetchMs = millis() - t0;
        Serial.printf("PreviewService: fetch %s: %s, %llu bytes in %lu ms\n", req.path,
                      outcome == Outcome::Ok ? "ok" : outcome == Outcome::Cancelled ? "cancelled" : failText,
                      (unsigned long long)got, (unsigned long)sum.fetchMs);
    } else {
        copy(localPath, sizeof(localPath), req.path);
    }

    if (outcome == Outcome::Ok) {
        setState(gen, PreviewService::ANALYSING);
        File f = SD.open(localPath, FILE_READ);
        if (!f) {
            outcome = fail("The file is not on the display SD.");
        } else {
            SdReader reader(f, &cancelFlag);
            if (!reader.ok()) {
                outcome = fail("Not enough memory to read the job.");
            } else {
                totalBytes = reader.fileSize();
                gcode::AnalysisOptions options;
                options.outline = true;
                options.progress = [](void *ctx, uint32_t, uint64_t) -> bool {
                    SdReader *r = (SdReader *)ctx;
                    doneBytes = r->position();
                    passNumber = r->passes();
                    return !cancelFlag;
                };
                options.progressContext = &reader;
                const uint32_t readBefore = reader.readMicros();
                gcode::Analysis a;
                uint32_t t0 = millis();
                activeReader = &reader;
                list.attach(listMemory, listBytes);
                notes.attach(notesMemory, NOTES_BYTES);
                SegmentList::Packer packer(list);
                TimedPacker timed(packer);
                estimateRatesGen = MachineService::ratesGeneration();
                gcode::Status s = gcode::analyse(reader, options, gcode::Arena{arena, ARENA_SIZE}, &timed, a);
                activeReader = nullptr;
                sum.analyseMs = millis() - t0;
                if (reader.cancelled() || s == gcode::Status::Cancelled) {
                    outcome = Outcome::Cancelled;
                } else if (s == gcode::Status::Refused) {
                    outcome = fail(a.refusedMessage ? a.refusedMessage : "The file is not a text file.");
                } else if (s == gcode::Status::LineTooLong) {
                    char text[160];
                    snprintf(text, sizeof(text), "Line %lu is too long for the preview (over %lu characters).",
                             (unsigned long)(a.longLine + 1), (unsigned long)options.maxLineLength);
                    outcome = fail(text);
                } else if (s != gcode::Status::Ok) {
                    outcome = fail(statusText(s));
                } else {
                    sum.lines = a.lineCount;
                    sum.inch = a.units == gcode::Units::In;
                    sum.hasExtents = a.displayExtents.present;
                    sum.minX = a.displayExtents.minX;
                    sum.maxX = a.displayExtents.maxX;
                    sum.minY = a.displayExtents.minY;
                    sum.maxY = a.displayExtents.maxY;
                    sum.outline = a.outlineStatus == gcode::Status::Ok;
                    sum.arenaPeak = (uint32_t)a.arenaPeak;
                    sum.fileBytes = reader.fileSize();
                    summarise(sum, a, reader.fileSize());
                }
                Serial.printf("PreviewService: analyse %s: status %d, %lu lines in %lu ms, outline %d, peak %lu bytes\n",
                              localPath, (int)s, (unsigned long)a.lineCount, (unsigned long)sum.analyseMs,
                              (int)a.outlineStatus, (unsigned long)a.arenaPeak);
                Serial.printf("PreviewService: of which SD reads %lu ms, packing %lu ms\n",
                              (unsigned long)((reader.readMicros() - readBefore) / 1000), (unsigned long)(timed.us / 1000));
                Serial.printf("PreviewService: %lu moves in %lu records of %lu bytes, step %.2f mm, Z step %.1f mm%s, "
                              "estimate %.0f s%s\n",
                              (unsigned long)list.moves(), (unsigned long)list.records(), (unsigned long)listBytes,
                              list.xyStep(), list.zStep(), list.truncated() ? ", truncated" : "",
                              sum.estimateSeconds, sum.estimateIncomplete ? "+" : "");
                Serial.printf("PreviewService: %lu messages%s, %d ops\n", (unsigned long)notes.messages(),
                              notes.full() ? " (no room for more)" : "", sum.ops);
                Serial.printf("PreviewService: %lu probes, %lu homings, %lu prompts: %.0f s allowed\n",
                              (unsigned long)sum.probes, (unsigned long)sum.homings, (unsigned long)sum.prompts,
                              sum.allowanceSeconds);
            }
        }
    }

    if (gen == generation) {
        if (outcome == Outcome::Ok) {
            result = sum;
            readyGen++;
        }
        setState(gen, outcome == Outcome::Ok          ? PreviewService::READY
                      : outcome == Outcome::Cancelled ? PreviewService::CANCELLED
                                                      : PreviewService::FAILED);
    }
    delete work;
    taskRunning = false;
    vTaskDelete(nullptr);
}

}  // namespace

// --- The service ----------------------------------------------------------------------------

PreviewService::State PreviewService::state() { return current; }
const PreviewService::Summary &PreviewService::summary() { return result; }
const SegmentList *PreviewService::segments() { return current == READY && !taskRunning ? &list : nullptr; }
const JobNotes *PreviewService::notes() { return current == READY && !taskRunning ? &::notes : nullptr; }
SegmentList::Rates PreviewService::rates() { return currentRates(); }
uint32_t PreviewService::estimateGeneration() { return estimateGen; }
uint32_t PreviewService::readyGeneration() { return readyGen; }
const char *PreviewService::jobPath() { return request.path; }
const char *PreviewService::jobName() { return request.name; }
int32_t PreviewService::jobSize() {
    if (request.size < 0 && current == READY && !taskRunning) return (int32_t)result.fileBytes;
    return request.size;
}

void PreviewService::load(FileService::Card card, const char *path, const char *name, int32_t size) {
    loadRequest(card, path, name, size, false, false);
}

void PreviewService::loadForRun(const char *path, const char *name, int32_t size, bool download) {
    loadRequest(FileService::CARD_CONTROLLER, path, name, size, true, !download);
}

void PreviewService::loadRequest(FileService::Card card, const char *path, const char *name, int32_t size,
                                 bool duringJob, bool useCopy) {
    request.duringJob = duringJob;
    request.useCopy = useCopy;
    request.card = card;
    copy(request.path, sizeof(request.path), path);
    copy(request.name, sizeof(request.name), name);
    request.size = size;
    generation = generation + 1;
    if (taskRunning) cancelFlag = true;
    pendingStart = true;
    doneBytes = 0;
    totalBytes = size > 0 ? (uint64_t)size : 0;
    passNumber = 1;
    current = card == FileService::CARD_CONTROLLER && !useCopy ? FETCHING : ANALYSING;
    poll();
}

void PreviewService::clear() {
    generation = generation + 1;
    if (taskRunning) cancelFlag = true;
    pendingStart = false;
    current = NONE;
}

void PreviewService::cancel() {
    if (current == WAITING_FOR_JOB || pendingStart) {
        pendingStart = false;
        current = CANCELLED;
    }
    if (taskRunning) cancelFlag = true;
}

void PreviewService::retry() {
    if (current == FAILED || current == CANCELLED || current == WAITING_FOR_JOB) {
        loadRequest(request.card, request.path, request.name, request.size, request.duringJob, request.useCopy);
    }
}

bool PreviewService::busy() { return taskRunning; }

bool PreviewService::releaseMemory() {
    if (taskRunning || pendingStart) return false;
    if (current == READY) current = CANCELLED;
    free(arena);
    arena = nullptr;
    list.detach();
    free(listMemory);
    listMemory = nullptr;
    listBytes = 0;
    return true;
}

float PreviewService::progress() {
    uint64_t total = totalBytes, done = doneBytes;
    uint32_t pass = passNumber;
    SdReader *r = activeReader;
    if (current == ANALYSING && r) {
        // Read across tasks without a lock: a torn value only moves the bar for a moment
        done = r->position();
        pass = r->passes();
    }
    if (!total) return 0;
    float f = (float)done / (float)total;
    if (f > 1) f = 1;
    // The analysis reads the file twice (15 §5.2): one bar over both passes
    if (current == ANALYSING) f = (pass >= 2 ? 0.5f : 0.0f) + f * 0.5f;
    return f;
}

const char *PreviewService::message() {
    static char out[sizeof(failText)];
    std::lock_guard<std::mutex> g(lock);
    copy(out, sizeof(out), failText);
    return out;
}

void PreviewService::start() {
    pendingStart = false;
    {
        std::lock_guard<std::mutex> g(lock);
        failText[0] = '\0';
    }
    if (!FileService::displayCardReady() || FileService::displayCardBusy()) {
        // An upload from the display SD is the card's only other long user; the preview waits
        // for it like for a job (Retry)
        current = FileService::displayCardBusy() ? FAILED : NO_CARD;
        if (current == FAILED) {
            std::lock_guard<std::mutex> g(lock);
            copy(failText, sizeof(failText), "The display SD is busy with an upload.");
        }
        return;
    }
    if (request.card == FileService::CARD_CONTROLLER) {
        if (MachineService::jobActive() && !request.duringJob) {
            // Reading the controller's card during a cut takes its time (D72): wait for the end
            current = WAITING_FOR_JOB;
            return;
        }
        if (!MachineService::online() && !request.useCopy) {
            current = FAILED;
            std::lock_guard<std::mutex> g(lock);
            copy(failText, sizeof(failText), "The controller is offline.");
            return;
        }
    }
    if (!arena) arena = heap_caps_malloc(ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!notesMemory) notesMemory = heap_caps_malloc(NOTES_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (arena && !listMemory) {
        // The largest free block less some headroom (02 §6, D89)
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        size_t bytes = largest > RUN_RESERVE + LIST_MIN ? largest - RUN_RESERVE : 0;
        if (bytes > LIST_MAX) bytes = LIST_MAX;
        if (bytes >= LIST_MIN) listMemory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        listBytes = listMemory ? bytes : 0;
        Serial.printf("PreviewService: segment list %lu bytes (largest free block %lu)\n", (unsigned long)listBytes,
                      (unsigned long)largest);
    }
    if (!arena || !listMemory || !notesMemory) {
        current = FAILED;
        std::lock_guard<std::mutex> g(lock);
        copy(failText, sizeof(failText), "Not enough free memory for the preview.");
        return;
    }
    current = request.card == FileService::CARD_CONTROLLER && !request.useCopy ? FETCHING : ANALYSING;
    doneBytes = 0;
    passNumber = 1;
    cancelFlag = false;
    taskRunning = true;
    Work *work = new Work{request, generation};
    // Core 0, like the other worker tasks (02 §7)
    if (xTaskCreatePinnedToCore(task, "preview", 16384, work, 1, nullptr, 0) != pdPASS) {
        delete work;
        taskRunning = false;
        current = FAILED;
        std::lock_guard<std::mutex> g(lock);
        copy(failText, sizeof(failText), "The preview task could not start.");
    }
}

void PreviewService::poll() {
    if (pendingStart && !taskRunning) start();
    if (current == WAITING_FOR_JOB && !taskRunning && !MachineService::jobActive()) start();
    // The axis rates arrived or changed after the analysis: the estimate again (a pass over the
    // list, milliseconds)
    if (current == READY && !taskRunning && MachineService::ratesGeneration() != estimateRatesGen) {
        estimateRatesGen = MachineService::ratesGeneration();
        SegmentList::Estimate e = list.estimate(currentRates());
        result.estimateSeconds = e.seconds;
        result.estimateFeedSeconds = e.feedSeconds;
        result.estimateIncomplete = e.incomplete;
        estimateGen++;
    }
}
