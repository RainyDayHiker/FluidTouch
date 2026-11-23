#pragma once

#include "gcode/io.h"
#include <SD.h>
#include <stdint.h>

// A file on the display SD, split into lines for the G-code library (CNC-Tools guide.md §1),
// shared by the library tests and the preview's analysis.
//
// \r\n, \n or \r ends a line; a last line may have no ending. A line longer than LINE_KEPT keeps
// its first LINE_KEPT bytes but reports its real length, so the library reports LineTooLong.
// Every line passes through SdReader::yieldNow() (02 §7).
class SdReader final : public gcode::LineReader {
public:
    static const size_t CHUNK = 16384;     // SD reads (several sectors per read)
    static const size_t LINE_KEPT = 2048;  // longest line kept whole

    // `cancel`, if given, is read at every line: once true, the reader ends the file with
    // failed() true, so the library stops with ReadError.
    explicit SdReader(File f, volatile bool *cancel = nullptr);
    ~SdReader();
    bool ok() const { return chunk && line; }

    bool next(gcode::Line &out) override;
    bool rewind() override;
    bool failed() const override { return readError; }

    // For a progress bar: bytes read in this pass, the file's size, passes started (1 at first)
    uint64_t position() const { return offset; }
    size_t fileSize() const { return size; }
    uint32_t passes() const { return pass; }
    bool cancelled() const { return stopped; }

    // Gives the core away for a tick every 50 ms. The library and the SD card's SPI reads never
    // wait, and a task on core 0 that doesn't yield starves the idle task until the task
    // watchdog reboots the device (seen on the device, 2026-11-19).
    static void yieldNow();
    // Time spent in SD reads since construction, microseconds
    uint32_t readMicros() const { return readUs; }

private:
    bool fill();
    File file;
    volatile bool *cancel;
    uint8_t *chunk = nullptr;
    char *line = nullptr;
    size_t have = 0, at = 0, size = 0;
    uint32_t index = 0;
    uint32_t pass = 1;
    uint64_t offset = 0;
    bool readError = false;
    uint32_t readUs = 0;
    bool stopped = false;
};
