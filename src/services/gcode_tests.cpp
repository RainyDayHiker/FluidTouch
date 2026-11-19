#include "services/gcode_tests.h"
#include "gcode/version.h"
#include "services/file_service.h"
#include "services/gcode_device/runner.h"
#include "services/job_service.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <mutex>
#include <stdio.h>
#include <string.h>

namespace GcodeTests {
namespace {

constexpr size_t ARENA_SIZE = 1024 * 1024;  // the display's budget (CNC-Tools docs/arena.md)
constexpr size_t CHUNK = 4096;               // SD reads
constexpr size_t LINE_KEPT = 2048;            // longest line kept whole; the data's lines are under 400

void *psram(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

void fullPath(char *out, size_t size, const char *rel) { snprintf(out, size, "%s/%s", DATA_FOLDER, rel); }

// The library and the runner never wait, and the SD card's SPI reads are busy-waits, so without
// this the task holds its core until the task watchdog reboots the device (seen on the device,
// 2026-11-19). Every line read passes here: give the core away for a tick every 50 ms. That
// lets the idle task and WiFi run, at about 2% of the perf group's times.
uint32_t lastYieldMs = 0;
void yieldNow() {
    uint32_t now = ::millis();
    if (now - lastYieldMs >= 50) {
        delay(1);
        lastYieldMs = ::millis();
    }
}

// A file on the display SD, split into lines as the library expects (CNC-Tools guide.md §1):
// \r\n, \n or \r ends a line; a last line may have no ending. A line longer than LINE_KEPT keeps
// its first LINE_KEPT bytes but reports its real length, so the library reports LineTooLong.
class SdReader final : public gcode::LineReader {
public:
    explicit SdReader(File f) : file(f) {
        chunk = (uint8_t *)psram(CHUNK);
        line = (char *)psram(LINE_KEPT);
        size = file.size();
    }
    ~SdReader() {
        file.close();
        free(chunk);
        free(line);
    }
    bool ok() const { return chunk && line; }

    bool next(gcode::Line &out) override {
        yieldNow();
        size_t n = 0;
        out.offset = offset;
        int c;
        while ((c = get()) >= 0 && c != '\n' && c != '\r') {
            if (n < LINE_KEPT) line[n] = (char)c;
            n++;
        }
        if (c < 0 && n == 0) return false;
        if (c < 0) {
            out.ending = gcode::Ending::None;
        } else if (c == '\n') {
            out.ending = gcode::Ending::LF;
        } else if (peek() == '\n') {
            get();
            out.ending = gcode::Ending::CRLF;
        } else {
            out.ending = gcode::Ending::CR;
        }
        out.data = line;
        out.length = (uint32_t)n;
        out.index = index++;
        return true;
    }
    bool rewind() override {
        have = at = 0;
        index = 0;
        offset = 0;
        readError = false;
        return file.seek(0);
    }
    bool failed() const override { return readError; }

private:
    bool fill() {
        if (at < have) return true;
        size_t pos = file.position();
        if (pos >= size) return false;
        have = file.read(chunk, CHUNK);
        at = 0;
        if (have == 0) readError = true;  // short of the end: an SD error
        return have > 0;
    }
    int get() {
        if (!fill()) return -1;
        offset++;
        return chunk[at++];
    }
    int peek() { return fill() ? chunk[at] : -1; }

    File file;
    uint8_t *chunk = nullptr;
    char *line = nullptr;
    size_t have = 0, at = 0, size = 0;
    uint32_t index = 0;
    uint64_t offset = 0;
    bool readError = false;
};

class SdWriter final : public gcode::ByteWriter {
public:
    explicit SdWriter(File f) : file(f) {}
    ~SdWriter() { file.close(); }
    bool write(const char *data, size_t length) override { return file.write((const uint8_t *)data, length) == length; }

private:
    File file;
};

// What the UI shows; written by the task, read by the UI, under the lock.
std::mutex lock;
volatile State current = State::Idle;
char status[200] = "Not run since start-up.";
char failure[200] = "";
unsigned passed = 0, failedCount = 0, skipped = 0;
uint32_t startedMs = 0;

void setStatus(const char *text) {
    std::lock_guard<std::mutex> g(lock);
    snprintf(status, sizeof(status), "%s", text);
}

class SdPlatform final : public gcode_device::Platform {
public:
    explicit SdPlatform(File results) : results(results) {}
    ~SdPlatform() { results.close(); }

    gcode::LineReader *open(const char *path) override {
        char p[160];
        fullPath(p, sizeof(p), path);
        File f = SD.open(p, FILE_READ);
        if (!f) return nullptr;
        SdReader *r = new SdReader(f);
        if (!r->ok()) {
            delete r;
            return nullptr;
        }
        return r;
    }
    void close(gcode::LineReader *reader) override { delete static_cast<SdReader *>(reader); }
    gcode::ByteWriter *create(const char *path) override {
        fullPath(writePath, sizeof(writePath), path);
        File f = SD.open(writePath, FILE_WRITE);
        return f ? new SdWriter(f) : nullptr;
    }
    void closeWriter(gcode::ByteWriter *writer) override {
        delete static_cast<SdWriter *>(writer);
        SD.remove(writePath);
    }
    void print(const char *text) override {
        if (results) {
            results.write((const uint8_t *)text, strlen(text));
            results.write((const uint8_t *)"\n", 1);
            results.flush();
        }
        Serial.printf("GcodeTests: %s\n", text);
        std::lock_guard<std::mutex> g(lock);
        if (strncmp(text, "PASS", 4) == 0) passed++;
        if (strncmp(text, "SKIP", 4) == 0) skipped++;
        if (strncmp(text, "FAIL", 4) == 0) {
            if (failedCount == 0) snprintf(failure, sizeof(failure), "%s", text);
            failedCount++;
        }
        if (strncmp(text, "SUMMARY", 7) != 0) {
            snprintf(status, sizeof(status), "Running: %u passed, %u failed, %u skipped so far", passed, failedCount,
                     skipped);
        }
    }
    uint32_t millis() override { return ::millis(); }

private:
    File results;
    char writePath[160] = "";
};

struct Run {
    void *arena;
    File results;
};

void task(void *param) {
    Run *run = (Run *)param;
    gcode_device::Totals t;
    {
        SdPlatform platform(run->results);
        t = gcode_device::run(platform, gcode::Arena{run->arena, ARENA_SIZE}, nullptr);
    }
    free(run->arena);
    delete run;
    uint32_t s = (::millis() - startedMs) / 1000;
    char text[200];
    snprintf(text, sizeof(text), "%s: %u passed, %u failed, %u skipped in %u:%02u\ngcode-core %s, results in %s/results.txt",
             t.failed == 0 && t.passed > 0 ? "Passed" : "FAILED", (unsigned)t.passed, (unsigned)t.failed,
             (unsigned)t.skipped, (unsigned)(s / 60), (unsigned)(s % 60), gcode::version(), DATA_FOLDER);
    setStatus(text);
    Serial.printf("GcodeTests: %s\n", text);
    current = State::Done;
    vTaskDelete(nullptr);
}

bool refuse(const char *why) {
    setStatus(why);
    current = State::Error;
    return false;
}

}  // namespace

bool start() {
    if (current == State::Running) return false;
    if (JobService::active()) return refuse("Not while a job runs.");
    if (!FileService::displayCardReady()) return refuse("No display SD.");
    char manifest[160];
    fullPath(manifest, sizeof(manifest), "manifest.tsv");
    if (!SD.exists(manifest)) {
        return refuse("No test data on the display SD.\nCopy the release's device-tests/data/ to /gcode-tests/.");
    }
    void *arena = heap_caps_malloc(ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arena) return refuse("Not enough free PSRAM for the 1 MB arena.");
    char resultsPath[160];
    fullPath(resultsPath, sizeof(resultsPath), "results.txt");
    File results = SD.open(resultsPath, FILE_WRITE);
    {
        std::lock_guard<std::mutex> g(lock);
        passed = failedCount = skipped = 0;
        failure[0] = '\0';
        snprintf(status, sizeof(status), "Starting...");
    }
    startedMs = ::millis();
    current = State::Running;
    Run *run = new Run{arena, results};
    // Core 0, like the connect task; 16 KB of stack (the runner keeps its big buffers static)
    if (xTaskCreatePinnedToCore(task, "gcode_tests", 16384, run, 1, nullptr, 0) != pdPASS) {
        results.close();
        free(arena);
        delete run;
        return refuse("Could not start the test task.");
    }
    Serial.printf("GcodeTests: running gcode-core %s from %s\n", gcode::version(), DATA_FOLDER);
    return true;
}

State state() { return current; }

void statusText(char *out, unsigned size) {
    std::lock_guard<std::mutex> g(lock);
    snprintf(out, size, "%s", status);
}

void firstFailure(char *out, unsigned size) {
    std::lock_guard<std::mutex> g(lock);
    snprintf(out, size, "%s", failure);
}

}  // namespace GcodeTests
