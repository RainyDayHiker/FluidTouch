#include "services/sd_reader.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {

void *psram(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

uint32_t lastYieldMs = 0;

}  // namespace

void SdReader::yieldNow() {
    uint32_t now = millis();
    if (now - lastYieldMs >= 50) {
        delay(1);
        lastYieldMs = millis();
    }
}

SdReader::SdReader(File f, volatile bool *cancelFlag) : file(f), cancel(cancelFlag) {
    chunk = (uint8_t *)psram(CHUNK);
    line = (char *)psram(LINE_KEPT);
    size = file.size();
}

SdReader::~SdReader() {
    file.close();
    free(chunk);
    free(line);
}

// Scans the chunk for the next line end and copies the bytes before it in one go. Reading a
// byte at a time made reading about half of analyse()'s time on the device (06, "Device test
// suite runs").
bool SdReader::next(gcode::Line &out) {
    yieldNow();
    if (cancel && *cancel) {
        stopped = true;
        readError = true;
        return false;
    }
    size_t n = 0;
    out.offset = offset;
    int end = -1;  // the byte that ended the line, or -1 at the end of the file
    while (fill()) {
        const uint8_t *p = chunk + at;
        size_t avail = have - at, i = 0;
        while (i < avail && p[i] != '\n' && p[i] != '\r') i++;
        if (n < LINE_KEPT) memcpy(line + n, p, i < LINE_KEPT - n ? i : LINE_KEPT - n);
        n += i;
        at += i;
        offset += i;
        if (i < avail) {
            end = chunk[at++];
            offset++;
            break;
        }
    }
    if (end < 0 && n == 0) return false;
    if (end < 0) {
        out.ending = gcode::Ending::None;
    } else if (end == '\n') {
        out.ending = gcode::Ending::LF;
    } else if (fill() && chunk[at] == '\n') {
        at++;
        offset++;
        out.ending = gcode::Ending::CRLF;
    } else {
        out.ending = gcode::Ending::CR;
    }
    out.data = line;
    out.length = (uint32_t)n;
    out.index = index++;
    return true;
}

bool SdReader::rewind() {
    have = at = 0;
    index = 0;
    offset = 0;
    readError = false;
    pass++;
    return file.seek(0);
}

bool SdReader::fill() {
    if (at < have) return true;
    size_t pos = file.position();
    if (pos >= size) return false;
    uint32_t t0 = micros();
    have = file.read(chunk, CHUNK);
    readUs += micros() - t0;
    at = 0;
    if (have == 0) readError = true;  // short of the end: an SD error
    return have > 0;
}
