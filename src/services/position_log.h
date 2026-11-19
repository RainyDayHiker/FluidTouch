#ifndef LRT_POSITION_LOG_H
#define LRT_POSITION_LOG_H

#include <stdint.h>

// Records a running job's status reports to a CSV on the display SD, when
// Settings > Tools has it on (specs/06-test-and-spikes.md S3, D62). One row
// per status report; rows are kept in RAM and written every 2 s or 4 KB.
class PositionLog {
public:
    // The last recording: its file, rows written, and why it stopped early
    // ("" if it did not)
    static const char *lastFile() { return fileName; }
    static uint32_t rows() { return rowCount; }
    static const char *error() { return errorText; }
    static bool recording() { return isRecording; }

    // Call from the main loop
    static void poll();

private:
    static void start();
    static void addRow();
    static void flush();
    static void finish(const char *why);

    static bool isRecording;
    static char fileName[128];
    static uint32_t rowCount;
    static char errorText[96];
};

#endif // LRT_POSITION_LOG_H
