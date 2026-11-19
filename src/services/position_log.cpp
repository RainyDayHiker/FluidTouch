#include "services/position_log.h"
#include "services/file_service.h"
#include "services/job_service.h"
#include "services/machine_service.h"
#include "services/settings_service.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>

bool PositionLog::isRecording = false;
char PositionLog::fileName[128] = "";
uint32_t PositionLog::rowCount = 0;
char PositionLog::errorText[96] = "";

namespace {

const char *const FOLDER = "/logs";
const uint32_t FLUSH_MS = 2000;
const size_t FLUSH_BYTES = 4096;
const int MAX_NUMBER = 999;

// Rows not yet written; one row is at most about 300 bytes
char buffer[FLUSH_BYTES + 512];
size_t used = 0;
uint32_t startMs = 0;
uint32_t lastFlushMs = 0;
uint32_t lastReportMs = 0;
char lastMessage[128] = "";
bool wasActive = false;

const char *stateName(MachineState s) {
    switch (s) {
        case STATE_IDLE: return "Idle";
        case STATE_RUN: return "Run";
        case STATE_HOLD: return "Hold";
        case STATE_JOG: return "Jog";
        case STATE_ALARM: return "Alarm";
        case STATE_DOOR: return "Door";
        case STATE_CHECK: return "Check";
        case STATE_HOME: return "Home";
        case STATE_SLEEP: return "Sleep";
        default: return "Unknown";
    }
}

// The job's name without its extension, with only safe characters
void baseFor(char *out, size_t outLen, const char *jobName) {
    size_t n = 0;
    const char *dot = strrchr(jobName, '.');
    for (const char *p = jobName; *p && p != dot && n < outLen - 1 && n < 40; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out[n++] = ok ? c : '_';
    }
    if (n == 0) {
        strncpy(out, "job", outLen - 1);
        n = strlen(out);
    }
    out[n] = '\0';
}

void append(const char *text) {
    size_t len = strlen(text);
    if (used + len >= sizeof(buffer)) len = sizeof(buffer) - 1 - used;
    memcpy(buffer + used, text, len);
    used += len;
}

}  // namespace

void PositionLog::start() {
    rowCount = 0;
    errorText[0] = '\0';
    fileName[0] = '\0';
    used = 0;
    if (!FileService::displayCardReady()) {
        finish("No display SD.");
        return;
    }
    if (!SD.exists(FOLDER) && !SD.mkdir(FOLDER)) {
        finish("Could not make /logs on the display SD.");
        return;
    }
    char base[48];
    baseFor(base, sizeof(base), JobService::runName());
    int number = 1;
    for (; number <= MAX_NUMBER; number++) {
        snprintf(fileName, sizeof(fileName), "%s/%s-%03d.csv", FOLDER, base, number);
        if (!SD.exists(fileName)) break;
    }
    if (number > MAX_NUMBER) {
        finish("Too many logs for this job in /logs.");
        return;
    }
    isRecording = true;
    startMs = millis();
    lastFlushMs = startMs;
    lastReportMs = MachineService::status().last_update_ms;
    // The first row carries the message current at the start: a job's first
    // (MSG ...) can arrive in the same loop as the start is noticed
    lastMessage[0] = '\0';
    append("ms,state,percent,wx,wy,wz,feed,message\n");
    Serial.printf("[PositionLog] Recording to %s\n", fileName);
    addRow();  // where it starts
}

void PositionLog::addRow() {
    const FluidNCStatus &s = MachineService::status();
    char row[320];
    char percent[16] = "";
    if (s.is_sd_printing) snprintf(percent, sizeof(percent), "%.2f", s.sd_percent);
    int n = snprintf(row, sizeof(row), "%lu,%s,%s,%.3f,%.3f,%.3f,%.0f,", (unsigned long)(millis() - startMs),
                     stateName(s.state), percent, s.wpos_x, s.wpos_y, s.wpos_z, s.feed_rate);
    // The message only when it changed, quoted, with " doubled
    if (strcmp(s.last_message, lastMessage) != 0) {
        strncpy(lastMessage, s.last_message, sizeof(lastMessage) - 1);
        lastMessage[sizeof(lastMessage) - 1] = '\0';
        if (n < (int)sizeof(row) - 4) row[n++] = '"';
        for (const char *p = lastMessage; *p && n < (int)sizeof(row) - 4; p++) {
            if (*p == '"') row[n++] = '"';
            row[n++] = *p;
        }
        row[n++] = '"';
    }
    row[n++] = '\n';
    row[n] = '\0';
    append(row);
    rowCount++;
}

void PositionLog::flush() {
    lastFlushMs = millis();
    if (!used) return;
    File f = SD.open(fileName, FILE_APPEND);
    if (!f) {
        used = 0;
        finish("Writing to the display SD failed.");
        return;
    }
    size_t written = f.write((const uint8_t *)buffer, used);
    f.close();
    bool ok = written == used;
    used = 0;
    if (!ok) finish("Writing to the display SD failed.");
}

void PositionLog::finish(const char *why) {
    if (isRecording) {
        isRecording = false;  // first, so a failing flush does not come back here
        // The rows still in RAM
        if (!why[0] && used) {
            File f = SD.open(fileName, FILE_APPEND);
            bool ok = f && f.write((const uint8_t *)buffer, used) == used;
            if (f) f.close();
            if (!ok) why = "Writing to the display SD failed.";
        }
        used = 0;
        Serial.printf("[PositionLog] %s: %lu rows%s%s\n", fileName, (unsigned long)rowCount, why[0] ? ", " : "", why);
    }
    strncpy(errorText, why, sizeof(errorText) - 1);
    errorText[sizeof(errorText) - 1] = '\0';
}

void PositionLog::poll() {
    bool active = JobService::active();
    bool on = SettingsService::positionLog();

    // A job starting is recorded once, from its start
    if (active && !wasActive && on) start();
    wasActive = active;
    if (!isRecording) return;

    if (!active) {
        finish("");
        return;
    }
    if (!on) {
        finish("Switched off during the job.");
        return;
    }

    const FluidNCStatus &s = MachineService::status();
    if (MachineService::online() && s.last_update_ms != lastReportMs) {
        lastReportMs = s.last_update_ms;
        addRow();
    }
    // The upload task reads the card: wait, unless the rows no longer fit
    bool busy = FileService::displayCardBusy();
    if (used >= FLUSH_BYTES || (!busy && millis() - lastFlushMs >= FLUSH_MS)) {
        if (busy) {
            finish("The display SD was busy too long.");
            return;
        }
        flush();
    }
}
