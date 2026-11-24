#pragma once

#include "gcode/analysis.h"
#include <stddef.h>
#include <stdint.h>

// The chosen job's segments, packed for the preview and for following the tool on Run
// (specs/11-preview.md §3, D30). Plain C++ with no Arduino calls, so a host test can check the
// packing against the library's segments.
//
// One block of memory: 8-byte records from the bottom, checkpoints from the top.
//
//   Move record:  end X, end Y (signed 16-bit steps of xyStep() mm from work zero); for an arc
//                 its centre's signed distance from the chord's middle, left of the chord
//                 positive (16-bit steps; exact for a half circle, where a radius isn't), else 0;
//                 meta:
//                 bits 0-1 kind, bit 2 fixed move, bits 3-10 Z level (signed 8-bit steps of
//                 zStep() mm from zOrigin()), bits 11-15 the line's distance from the previous
//                 move's line (0 to 30).
//   Extra record: meta bits 11-15 all set (31); bits 0-10 say which: the next move's absolute
//                 line, the centre of a full circle (or of an arc whose ends round together), a feed change, a dwell, a jump (the next move
//                 doesn't start where the last one ended: G92, G28, an unknown position), the
//                 next move's N word.
//
// A value that doesn't fit doubles its step and halves the records so far (a repack).
// Every 256 records a checkpoint holds the state before that record, so a reader can start
// there. Coordinates are mm (inch files are converted); feeds mm/min.
class SegmentList {
public:
    enum Kind : uint8_t { RAPID = 0, FEED = 1, ARC_CW = 2, ARC_CCW = 3 };
    enum FeedMode : uint8_t { FEED_NONE = 0, FEED_PER_MIN = 1, FEED_INVERSE_TIME = 2, FEED_PER_REV = 3 };

    static const uint32_t CHECKPOINT_EVERY = 256;
    static const size_t RECORD_BYTES = 8;

    // One move, decoded
    struct Move {
        Kind kind;
        bool fixed;      // a fixed positioning move (probe location, return to zero)
        bool jumped;     // starts away from the previous move's end
        float x0, y0, z0, x1, y1, z1;
        float cx, cy, r;  // arcs: centre and radius (r > 0)
        float sweep;      // arcs: signed sweep in radians, negative clockwise
        uint32_t line;    // 0-based
        float feed;       // mm/min, or for FEED_INVERSE_TIME 1/minutes
        FeedMode feedMode;
        float dwellBefore;  // seconds of G4 since the previous move
        uint32_t record;    // index of the move's record
    };

    struct Checkpoint {
        uint32_t line;     // the previous move's line
        uint32_t offset;   // its byte offset in the file
        uint32_t n;        // the last N word, 0 if none
        float feed;
        float seconds;      // the estimate up to here (estimate())
        float feedSeconds;  // of which feeds and arcs, without their dwells (the feed override's part)
        int16_t x, y;       // the previous move's end, in steps
        int8_t z;
        uint8_t feedMode;
        uint8_t pad[2];
    };

    // Axis rates for the estimate, mm/min; 0 when unknown
    struct Rates {
        float x, y, z;
    };
    struct Estimate {
        double seconds;
        double feedSeconds;  // of which feeds and arcs (dwells not included)
        bool incomplete;     // moves without a usable feed, or rates unknown
    };

    // Starts empty in `bytes` of memory owned by the caller
    void attach(void *memory, size_t bytes);
    void detach();
    bool attached() const { return mem != nullptr; }

    uint32_t records() const { return count; }
    uint32_t moves() const { return moveCount; }
    float xyStep() const { return xy; }
    float zStep() const { return zs; }
    float zOrigin() const { return z0; }
    // Memory ran out: only the moves before `truncatedOffset` (a file offset) are kept
    bool truncated() const { return full; }
    uint64_t truncatedOffset() const { return fullAt; }
    // Lowest and highest Z of every move, and of the moves that cut (feeds and arcs moving in
    // XY), in mm
    bool hasZ() const { return zSeen; }
    float zMin() const { return zLow; }
    float zMax() const { return zHigh; }
    bool hasCut() const { return cutSeen; }
    float cutTop() const { return cutHigh; }
    float cutBottom() const { return cutLow; }
    // A motion line moving in X or Y had an unknown position, so isn't in the list; not counted
    // when only its start was unknown (nothing to draw), nor a Z move (after a probe)
    bool skippedXY() const { return skipXY; }
    // N words increase through the file, so the moves' N words can be looked up (D79)
    bool nUsable() const { return nSeen && nIncreasing; }

    uint32_t checkpoints() const { return cpCount; }
    const Checkpoint &checkpoint(uint32_t i) const { return cps()[-(int32_t)i - 1]; }

    // Fixed moves seen (up to MAX_FIXED): their line and end, for the probe ring
    static const int MAX_FIXED = 8;
    int fixedCount() const { return fixedN; }
    uint32_t fixedLine(int i) const { return fixedLines[i]; }
    float fixedX(int i) const { return fixedXs[i]; }
    float fixedY(int i) const { return fixedYs[i]; }

    // The time the job takes (D80, spec 11 §5); also fills the checkpoints' seconds
    Estimate estimate(const Rates &rates);
    // One move's time in seconds, as estimate() counts it; `incomplete` is set when it had
    // no usable feed
    static double moveSeconds(const Move &m, const Rates &rates, bool &incomplete);
    // A move's share of feedSeconds: its time less its dwell for a feed or an arc, else 0
    static double feedPart(const Move &m, double seconds) { return m.kind == RAPID ? 0 : seconds - m.dwellBefore; }

    // Reads the moves in order, from the start or from a checkpoint
    class Reader {
    public:
        explicit Reader(const SegmentList &list, uint32_t fromCheckpoint = 0);
        bool next(Move &out);
        // Index of the next record to read
        uint32_t position() const { return at; }
        // G4 after the last move
        float trailingDwell() const { return dwell; }

    private:
        const SegmentList &list;
        uint32_t at;
        uint32_t line;
        int32_t px, py, pz;  // previous end in steps
        float feed;
        FeedMode feedMode;
        float dwell;
        bool haveLine;
        uint32_t nextLine;
        bool haveCentre;
        int32_t centreX, centreY;
        bool jumped;
    };

    // Receives the analysis's segments and lines (AnalysisSink) and packs them
    class Packer : public gcode::AnalysisSink {
    public:
        explicit Packer(SegmentList &list) : list(list) {}
        void segment(const gcode::Segment &s) override;
        void line(const gcode::Line &l, const gcode::Token *tokens, uint16_t tokenCount,
                  const gcode::LineState &state) override;

    private:
        SegmentList &list;
        bool pending = false;
        gcode::Segment seg = {};
    };

private:
    friend class Reader;
    friend class Packer;

    struct Record {
        int16_t a, b, c;
        uint16_t meta;
    };
    enum Extra : uint16_t { X_LINE = 1, X_CENTRE = 2, X_FEED = 3, X_DWELL = 4, X_JUMP = 5, X_N = 6 };
    static const uint16_t EXTRA_MARK = 31;

    Record *recs() const { return (Record *)mem; }
    Checkpoint *cps() const { return (Checkpoint *)((uint8_t *)mem + size); }  // grows down

    // Packing
    void add(const gcode::Segment &s, const gcode::LineState &st, uint32_t n, bool hasN);
    bool room(uint32_t extra) const;
    void put(int16_t a, int16_t b, int16_t c, uint16_t meta);
    void putExtra(Extra kind, int16_t a, int16_t b, int16_t c);
    void putExtra32(Extra kind, uint32_t v, int16_t c);
    int32_t toXY(double mm) const;
    int32_t toZ(double mm) const;
    void doubleXY();
    void doubleZ();

    void *mem = nullptr;
    size_t size = 0;
    uint32_t count = 0;
    uint32_t moveCount = 0;
    uint32_t cpCount = 0;
    float xy = 0.01f;
    float zs = 0.1f;
    float z0 = 0;
    bool zOriginSet = false;
    bool full = false;
    uint64_t fullAt = 0;

    // The packer's state: the last move written
    uint32_t lastLine = 0;
    uint32_t lastOffset = 0;
    double lastX = 0, lastY = 0, lastZ = 0;  // mm
    int16_t lastSx = 0, lastSy = 0;
    int8_t lastSz = 0;
    float lastFeed = 0;
    FeedMode lastFeedMode = FEED_NONE;
    float feedNow = 0;       // the modal F, mm/min
    bool feedKnown = false;
    uint32_t lastN = 0;      // the last N written
    uint32_t lastNAny = 0;   // the last N on any line
    bool nSeen = false;
    bool nIncreasing = true;
    float pendingDwell = 0;

    bool skipXY = false;
    bool zSeen = false, cutSeen = false;
    float zLow = 0, zHigh = 0, cutLow = 0, cutHigh = 0;

    int fixedN = 0;
    uint32_t fixedLines[MAX_FIXED];
    float fixedXs[MAX_FIXED], fixedYs[MAX_FIXED];
};
