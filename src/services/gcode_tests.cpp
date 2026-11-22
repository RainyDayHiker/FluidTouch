#include "services/gcode_tests.h"
#include "gcode/version.h"
#include "services/file_service.h"
#include "services/gcode_device/runner.h"
#include "services/job_service.h"
#include "services/preview_service.h"
#include "services/sd_reader.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <mutex>
#include <stdio.h>
#include <string.h>

namespace GcodeTests {
namespace {

constexpr size_t ARENA_SIZE = 1024 * 1024;  // the display's budget (CNC-Tools docs/arena.md)

void fullPath(char *out, size_t size, const char *rel) { snprintf(out, size, "%s/%s", DATA_FOLDER, rel); }

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
    // The perf group's write test is removed; a transform's mismatched output
    // (mismatch-<case>-<kind>.gcode) stays on the card to be compared
    void closeWriter(gcode::ByteWriter *writer) override {
        delete static_cast<SdWriter *>(writer);
        // the runner's timing files (perf-write.tmp, perf-surfacing.tmp) are scratch; a
        // mismatch-*.gcode file is kept for the report
        const size_t len = strlen(writePath);
        if (len > 4 && strcmp(writePath + len - 4, ".tmp") == 0) SD.remove(writePath);
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
    // The preview's 512 KB arena and this 1 MB one don't both fit (02 §6)
    if (!PreviewService::releaseMemory()) return refuse("Wait for the job's preview to finish.");
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
