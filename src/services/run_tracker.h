#pragma once

#include "services/segment_list.h"
#include <stdint.h>

// Finds the move the tool is on (specs/11-preview.md §6, D79). Plain C++ with no Arduino calls,
// for the host test.
//
// From FluidNC's `Ln:` (the N word of the move it is executing) when the file's N words increase:
// the move with that N, found from the checkpoints. Otherwise, or when no move has that N, by
// position matching (spike S3): the window runs from the last matched move to the moves FluidNC has read (the `SD:` percent as
// a byte offset, bounded by the segment list's checkpoints). The match is the first move in the
// window that passes within XY_TOLERANCE of the position in XY and Z_TOLERANCE (or one Z level)
// in Z. No match keeps the last one, so a jog in a hold or a probe changes nothing, and the
// match only moves forward: a path that retraces itself stays on the earliest pass.
class RunTracker {
public:
    static constexpr float XY_TOLERANCE = 1.0f;  // mm
    static constexpr float Z_TOLERANCE = 0.5f;   // mm
    // Unknown read position (FluidNC drops the SD: field once it has read the last line)
    static const uint64_t READ_ALL = ~(uint64_t)0;

    // A new run: back to the start of the list
    void reset();
    // A status report. Position in mm (work coordinates); readOffset in bytes, or READ_ALL;
    // lineN the report's `Ln:`, 0 when it has none. True when the match moved
    bool update(const SegmentList &list, float x, float y, float z, uint64_t readOffset, uint32_t lineN = 0);

    bool matched() const { return have; }
    // The matched move's record, and its line (0-based)
    uint32_t record() const { return rec; }
    uint32_t line() const { return ln; }
    // The last match came from `Ln:`
    bool byLineNumber() const { return byN; }

    // The first record past what FluidNC has read
    static uint32_t windowEnd(const SegmentList &list, uint64_t readOffset);
    // The record of the move whose N word is `n`, or NOT_FOUND (the list's N words must increase)
    static const uint32_t NOT_FOUND = ~(uint32_t)0;
    static uint32_t findN(const SegmentList &list, uint32_t n, uint32_t *line);
    // The move passes within the tolerances of the point
    static bool near(const SegmentList::Move &m, float x, float y, float z, float zTolerance);

private:
    bool have = false;
    uint32_t rec = 0;
    uint32_t ln = 0;
    bool byN = false;
};
