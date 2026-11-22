#include "services/segment_list.h"
#include "gcode/num.h"
#include "gcode/tokens.h"
#include <math.h>
#include <string.h>

namespace {

const double kPi = 3.14159265358979323846;
const int32_t XY_LIMIT = 32767;
const int32_t Z_LIMIT = 127;
const uint16_t X_NOP = 0;  // padding, so a move and its extras never straddle a checkpoint
const float NO_RATE_MM_MIN = 5000.0f;  // a move's speed when the axis rates are unknown (spec 11 §5)

// Halved, rounded half away from zero; a non-zero value stays non-zero
int16_t half(int32_t v) {
    if (v == 0) return 0;
    int32_t h = v > 0 ? (v + 1) / 2 : -((-v + 1) / 2);
    return (int16_t)h;
}

double wordValue(const gcode::Line &l, const gcode::Token &t) {
    uint32_t end = (uint32_t)t.start + t.length;
    if (t.valueStart >= end || end > l.length) return NAN;
    return gcode::parseDecimal(l.data + t.valueStart, end - t.valueStart);
}

}  // namespace

// --- Memory ---------------------------------------------------------------------------------

void SegmentList::attach(void *memory, size_t bytes) {
    *this = SegmentList();
    mem = memory;
    size = bytes & ~(size_t)7;
}

void SegmentList::detach() { *this = SegmentList(); }

bool SegmentList::room(uint32_t n) const {
    uint64_t recordsAfter = (uint64_t)count + n;
    uint64_t checkpointsAfter = (recordsAfter + CHECKPOINT_EVERY - 1) / CHECKPOINT_EVERY;
    return recordsAfter * RECORD_BYTES + checkpointsAfter * sizeof(Checkpoint) <= size;
}

void SegmentList::put(int16_t a, int16_t b, int16_t c, uint16_t meta) {
    if (count % CHECKPOINT_EVERY == 0) {
        Checkpoint &cp = cps()[-(int32_t)cpCount - 1];
        memset(&cp, 0, sizeof(cp));
        cp.line = lastLine;
        cp.offset = lastOffset;
        cp.n = lastN;
        cp.feed = lastFeed;
        cp.feedMode = lastFeedMode;
        cp.x = lastSx;
        cp.y = lastSy;
        cp.z = lastSz;
        cpCount++;
    }
    recs()[count++] = Record{a, b, c, meta};
}

void SegmentList::putExtra(Extra kind, int16_t a, int16_t b, int16_t c) {
    put(a, b, c, (uint16_t)((EXTRA_MARK << 11) | kind));
}

void SegmentList::putExtra32(Extra kind, uint32_t v, int16_t c) {
    putExtra(kind, (int16_t)(uint16_t)(v & 0xFFFF), (int16_t)(uint16_t)(v >> 16), c);
}

// In float: the ESP32-S3's FPU has no doubles
int32_t SegmentList::toXY(double mm) const { return (int32_t)lroundf((float)mm / xy); }
int32_t SegmentList::toZ(double mm) const { return (int32_t)lroundf(((float)mm - z0) / zs); }

void SegmentList::doubleXY() {
    xy *= 2;
    Record *r = recs();
    for (uint32_t i = 0; i < count; i++) {
        uint16_t mark = r[i].meta >> 11;
        uint16_t kind = r[i].meta & 0x7FF;
        if (mark != EXTRA_MARK) {
            r[i].a = half(r[i].a);
            r[i].b = half(r[i].b);
            r[i].c = half(r[i].c);
        } else if (kind == X_CENTRE || kind == X_JUMP) {
            r[i].a = half(r[i].a);
            r[i].b = half(r[i].b);
        }
    }
    for (uint32_t i = 0; i < cpCount; i++) {
        Checkpoint &cp = cps()[-(int32_t)i - 1];
        cp.x = half(cp.x);
        cp.y = half(cp.y);
    }
    lastSx = half(lastSx);
    lastSy = half(lastSy);
}

void SegmentList::doubleZ() {
    zs *= 2;
    Record *r = recs();
    for (uint32_t i = 0; i < count; i++) {
        uint16_t mark = r[i].meta >> 11;
        if (mark != EXTRA_MARK) {
            int8_t z = (int8_t)((r[i].meta >> 3) & 0xFF);
            int8_t h = (int8_t)half(z);
            r[i].meta = (uint16_t)((r[i].meta & ~(uint16_t)(0xFF << 3)) | ((uint16_t)(uint8_t)h << 3));
        } else if ((r[i].meta & 0x7FF) == X_JUMP) {
            r[i].c = half(r[i].c);
        }
    }
    for (uint32_t i = 0; i < cpCount; i++) {
        Checkpoint &cp = cps()[-(int32_t)i - 1];
        cp.z = (int8_t)half(cp.z);
    }
    lastSz = (int8_t)half(lastSz);
}

// --- Packing --------------------------------------------------------------------------------

void SegmentList::Packer::segment(const gcode::Segment &s) {
    // The line's tokens and state come in line(), just after: the move is packed there
    seg = s;
    pending = true;
}

void SegmentList::Packer::line(const gcode::Line &l, const gcode::Token *tokens, uint16_t tokenCount,
                               const gcode::LineState &st) {
    SegmentList &L = list;
    const double unit = st.units == gcode::UnitsMode::G20 ? 25.4 : 1.0;
    bool hasN = false;
    uint32_t n = 0;
    double dwell = 0;
    bool g4 = false;
    double p = NAN;
    // Only a line with F, N or P has anything to read: the others skip the tokens (parsing every
    // line's G word took most of the packing time on the device, without an FPU for doubles)
    const uint32_t wanted = (1u << ('F' - 'A')) | (1u << ('N' - 'A')) | (1u << ('P' - 'A'));
    if (!(st.letters & wanted)) tokenCount = 0;
    for (uint16_t i = 0; tokens && i < tokenCount; i++) {
        const gcode::Token &t = tokens[i];
        if (t.kind != gcode::TokenKind::Word) continue;
        if (t.letter == 'F') {
            double f = wordValue(l, t);
            if (!isnan(f)) {
                // F in inverse time (G93) is 1/minutes whatever the units
                L.feedNow = (float)(st.feedMode == gcode::FeedMode::G93 ? f : f * unit);
                L.feedKnown = true;
            }
        } else if (t.letter == 'N') {
            double v = wordValue(l, t);
            if (!isnan(v) && v >= 0 && v < 4294967295.0) {
                hasN = true;
                n = (uint32_t)v;
            }
        } else if (t.letter == 'G') {
            if (wordValue(l, t) == 4) g4 = true;
        } else if (t.letter == 'P') {
            p = wordValue(l, t);
        }
    }
    if (hasN) {
        if (L.nSeen && n <= L.lastNAny) L.nIncreasing = false;
        L.nSeen = true;
        L.lastNAny = n;
    }
    if (g4 && !isnan(p) && p > 0) dwell = p;

    bool moved = pending && seg.line == l.index;
    if (moved && !L.full) L.add(seg, st, hasN ? n : 0, hasN);
    // A move in XY the library gave no segment for (an unknown position): something is missing
    // from the picture, unless only its start was unknown (the first move of most files, from
    // wherever the machine is, which has nothing to draw)
    if (!moved && static_cast<int>(st.motion) <= 3 && st.isXYMotion) {
        bool startUnknown = isnan(st.before.x) || isnan(st.before.y);
        bool endKnown = !isnan(st.after.x) && !isnan(st.after.y);
        if (!(startUnknown && endKnown)) L.skipXY = true;
    }
    pending = false;
    if (dwell > 0) L.pendingDwell += (float)dwell;
}

void SegmentList::add(const gcode::Segment &s, const gcode::LineState &st, uint32_t n, bool hasN) {
    const double unit = st.units == gcode::UnitsMode::G20 ? 25.4 : 1.0;
    double x0 = s.x0 * unit, y0 = s.y0 * unit, sz0 = s.z0 * unit;
    double x1 = s.x1 * unit, y1 = s.y1 * unit, sz1 = s.z1 * unit;

    Kind kind = (Kind)s.kind;
    bool arc = (kind == ARC_CW || kind == ARC_CCW) && st.arc.state == gcode::ArcState::Ok && !isnan(s.cx) &&
               !isnan(s.cy);
    if (!arc && kind != RAPID) kind = FEED;  // G18/G19 and uninterpreted arcs: a straight line
    // An arc is stored as the centre's signed distance from the chord's middle (left of the
    // chord positive), which stays exact for a half circle where a radius would not; a full
    // circle, or one whose ends round to the same step, stores its centre instead
    double cx = 0, cy = 0, r = 0, h = 0;
    bool closed = false;
    if (arc) {
        cx = s.cx * unit;
        cy = s.cy * unit;
        r = hypot(x0 - cx, y0 - cy);
        closed = fabs(st.arc.geometry.sweep) > 2 * kPi - 1e-9;
        double dx = x1 - x0, dy = y1 - y0, d = hypot(dx, dy);
        if (d > 0) {
            h = ((cx - (x0 + x1) * 0.5) * -dy + (cy - (y0 + y1) * 0.5) * dx) / d;
        } else {
            closed = true;
        }
    }

    if (!zOriginSet) {
        z0 = (float)sz0;
        zOriginSet = true;
        lastZ = sz0;
    }
    bool jump = fabs(x0 - lastX) > 1e-6 || fabs(y0 - lastY) > 1e-6 || fabs(sz0 - lastZ) > 1e-6;

    FeedMode mode = FEED_NONE;
    float feed = 0;
    if (kind != RAPID) {
        if (st.feedMode == gcode::FeedMode::G93) {
            mode = FEED_INVERSE_TIME;
        } else if (st.feedMode == gcode::FeedMode::G95) {
            mode = FEED_PER_REV;
        } else if (feedKnown) {
            mode = FEED_PER_MIN;
        }
        feed = mode == FEED_NONE ? 0 : feedNow;
    }
    bool feedChange = kind != RAPID && (mode != lastFeedMode || feed != lastFeed);
    bool nRecord = hasN && nIncreasing;
    bool lineRecord = s.line < lastLine || s.line - lastLine > 30;
    bool dwellRecord = pendingDwell > 0;

    // Steps: double until every value fits
    int32_t sx, sy, sh, scx, scy, sjx, sjy;
    bool useCentre;
    for (;;) {
        sx = toXY(x1);
        sy = toXY(y1);
        sjx = jump ? toXY(x0) : lastSx;
        sjy = jump ? toXY(y0) : lastSy;
        int32_t chordX = sx - sjx, chordY = sy - sjy;
        useCentre = arc && (closed || chordX * chordX + chordY * chordY < 4);
        sh = arc && !useCentre ? toXY(h) : 0;
        scx = useCentre ? toXY(cx) : 0;
        scy = useCentre ? toXY(cy) : 0;
        const int32_t values[] = {sx, sy, sh, scx, scy, sjx, sjy};
        bool fits = true;
        for (int32_t v : values) fits = fits && v >= -XY_LIMIT && v <= XY_LIMIT;
        if (fits) break;
        doubleXY();
    }
    int32_t sz, sjz = 0;
    for (;;) {
        sz = toZ(sz1);
        if (jump) sjz = toZ(sz0);
        if (sz >= -Z_LIMIT && sz <= Z_LIMIT && sjz >= -Z_LIMIT && sjz <= Z_LIMIT) break;
        doubleZ();
    }
    if (arc && r < xy) {
        arc = useCentre = false;  // smaller than a step: a straight line
        sh = 0;
        kind = FEED;
    }

    uint32_t group = 1 + (nRecord ? 1 : 0) + (feedChange ? 1 : 0) + (lineRecord ? 1 : 0) + (dwellRecord ? 1 : 0) +
                     (jump ? 1 : 0) + (useCentre ? 1 : 0);
    uint32_t inBlock = count % CHECKPOINT_EVERY;
    uint32_t pad = inBlock != 0 && inBlock + group > CHECKPOINT_EVERY ? CHECKPOINT_EVERY - inBlock : 0;
    if (!room(pad + group)) {
        full = true;
        fullAt = s.offset;
        return;
    }
    for (uint32_t i = 0; i < pad; i++) putExtra((Extra)X_NOP, 0, 0, 0);

    if (nRecord) {
        putExtra32(X_N, n, 0);
        lastN = n;
    }
    if (dwellRecord) {
        uint32_t bits;
        memcpy(&bits, &pendingDwell, 4);
        putExtra32(X_DWELL, bits, 0);
        pendingDwell = 0;
    }
    if (feedChange) {
        uint32_t bits;
        memcpy(&bits, &feed, 4);
        putExtra32(X_FEED, bits, (int16_t)mode);
        lastFeed = feed;
        lastFeedMode = mode;
    }
    if (jump) {
        putExtra(X_JUMP, (int16_t)sjx, (int16_t)sjy, (int16_t)sjz);
        lastSx = (int16_t)sjx;
        lastSy = (int16_t)sjy;
        lastSz = (int8_t)sjz;
    }
    if (lineRecord) putExtra32(X_LINE, s.line, 0);
    if (useCentre) putExtra(X_CENTRE, (int16_t)scx, (int16_t)scy, 0);

    uint32_t delta = lineRecord ? 0 : s.line - lastLine;
    bool fixed = (s.flags & gcode::kSegmentFixedMove) != 0;
    uint16_t meta = (uint16_t)((delta << 11) | ((uint16_t)(uint8_t)(int8_t)sz << 3) | (fixed ? 4 : 0) | kind);
    put((int16_t)sx, (int16_t)sy, (int16_t)sh, meta);
    moveCount++;

    lastLine = s.line;
    lastOffset = (uint32_t)s.offset;
    lastX = x1;
    lastY = y1;
    lastZ = sz1;
    lastSx = (int16_t)sx;
    lastSy = (int16_t)sy;
    lastSz = (int8_t)sz;

    // Figures for the info line and the depth colours
    float lo = (float)(sz0 < sz1 ? sz0 : sz1), hi = (float)(sz0 < sz1 ? sz1 : sz0);
    if (!zSeen || lo < zLow) zLow = lo;
    if (!zSeen || hi > zHigh) zHigh = hi;
    zSeen = true;
    bool movesXY = arc || fabs(x1 - x0) > 1e-6 || fabs(y1 - y0) > 1e-6;
    if (kind != RAPID && movesXY) {
        if (!cutSeen || lo < cutLow) cutLow = lo;
        if (!cutSeen || hi > cutHigh) cutHigh = hi;
        cutSeen = true;
    }
    if (fixed && fixedN < MAX_FIXED) {
        fixedLines[fixedN] = s.line;
        fixedXs[fixedN] = (float)x1;
        fixedYs[fixedN] = (float)y1;
        fixedN++;
    }
}

// --- Reading --------------------------------------------------------------------------------

SegmentList::Reader::Reader(const SegmentList &l, uint32_t fromCheckpoint) : list(l) {
    at = 0;
    line = 0;
    px = py = pz = 0;
    feed = 0;
    feedMode = FEED_NONE;
    if (fromCheckpoint < l.cpCount) {
        const Checkpoint &cp = l.checkpoint(fromCheckpoint);
        at = fromCheckpoint * CHECKPOINT_EVERY;
        line = cp.line;
        px = cp.x;
        py = cp.y;
        pz = cp.z;
        feed = cp.feed;
        feedMode = (FeedMode)cp.feedMode;
    }
    dwell = 0;
    haveLine = false;
    nextLine = 0;
    haveCentre = false;
    centreX = centreY = 0;
    jumped = false;
}

bool SegmentList::Reader::next(Move &m) {
    const Record *r = list.recs();
    const float xy = list.xy, zs = list.zs, z0 = list.z0;
    while (at < list.count) {
        const Record &rec = r[at++];
        uint16_t mark = rec.meta >> 11;
        if (mark == EXTRA_MARK) {
            uint32_t v32 = (uint32_t)(uint16_t)rec.a | ((uint32_t)(uint16_t)rec.b << 16);
            switch (rec.meta & 0x7FF) {
                case X_LINE: haveLine = true; nextLine = v32; break;
                case X_CENTRE: haveCentre = true; centreX = rec.a; centreY = rec.b; break;
                case X_FEED: memcpy(&feed, &v32, 4); feedMode = (FeedMode)rec.c; break;
                case X_DWELL: { float d; memcpy(&d, &v32, 4); dwell += d; break; }
                case X_JUMP: px = rec.a; py = rec.b; pz = rec.c; jumped = true; break;
                default: break;  // padding, N words
            }
            continue;
        }
        m.kind = (Kind)(rec.meta & 3);
        m.fixed = (rec.meta & 4) != 0;
        m.jumped = jumped;
        int32_t z = (int8_t)((rec.meta >> 3) & 0xFF);
        m.x0 = px * xy;
        m.y0 = py * xy;
        m.z0 = z0 + pz * zs;
        m.x1 = rec.a * xy;
        m.y1 = rec.b * xy;
        m.z1 = z0 + z * zs;
        line = haveLine ? nextLine : line + mark;
        m.line = line;
        m.feed = feed;
        m.feedMode = feedMode;
        m.dwellBefore = dwell;
        m.record = at - 1;
        m.cx = m.cy = m.r = m.sweep = 0;
        if (m.kind == ARC_CW || m.kind == ARC_CCW) {
            bool ccw = m.kind == ARC_CCW;
            float dx = m.x1 - m.x0, dy = m.y1 - m.y0;
            float d = sqrtf(dx * dx + dy * dy);
            if (haveCentre) {
                m.cx = centreX * xy;
                m.cy = centreY * xy;
            } else if (d > 0) {
                float h = rec.c * xy;
                m.cx = (m.x0 + m.x1) * 0.5f - h * dy / d;
                m.cy = (m.y0 + m.y1) * 0.5f + h * dx / d;
            }
            m.r = hypotf(m.x0 - m.cx, m.y0 - m.cy);
            if ((!haveCentre && d <= 0) || m.r <= 0) {
                m.kind = FEED;  // can't be placed: drawn straight
            } else {
                // Ends that coincide make a full circle
                float a0 = atan2f(m.y0 - m.cy, m.x0 - m.cx);
                float a1 = atan2f(m.y1 - m.cy, m.x1 - m.cx);
                float sweep = a1 - a0;
                if (ccw) {
                    while (sweep <= 1e-6f) sweep += (float)(2 * kPi);
                } else {
                    while (sweep >= -1e-6f) sweep -= (float)(2 * kPi);
                }
                m.sweep = sweep;
            }
        }
        px = rec.a;
        py = rec.b;
        pz = z;
        dwell = 0;
        haveLine = false;
        haveCentre = false;
        jumped = false;
        return true;
    }
    return false;
}

// --- The estimate (D80) ---------------------------------------------------------------------

double SegmentList::moveSeconds(const Move &m, const Rates &rates, bool &incomplete) {
    double dx = m.x1 - m.x0, dy = m.y1 - m.y0, dz = m.z1 - m.z0;
    bool arc = m.kind == ARC_CW || m.kind == ARC_CCW;
    double xyLen = arc ? fabs((double)m.sweep) * m.r : sqrt(dx * dx + dy * dy);
    double len = sqrt(xyLen * xyLen + dz * dz);
    bool haveRates = rates.x > 0 && rates.y > 0 && rates.z > 0;
    double floor = 0;  // minutes: no axis faster than its max rate
    if (haveRates) {
        double ax = arc ? xyLen / (rates.x < rates.y ? rates.x : rates.y)
                        : fmax(fabs(dx) / rates.x, fabs(dy) / rates.y);
        floor = fmax(ax, fabs(dz) / rates.z);
    } else {
        incomplete = true;
    }
    double minutes;
    if (m.kind == RAPID) {
        minutes = haveRates ? floor : len / NO_RATE_MM_MIN;
    } else if (m.feedMode == FEED_PER_MIN && m.feed > 0) {
        minutes = fmax(len / m.feed, floor);
    } else if (m.feedMode == FEED_INVERSE_TIME && m.feed > 0) {
        minutes = fmax(1.0 / m.feed, floor);
    } else {
        // G95, or no F yet: as fast as the axes go
        minutes = haveRates ? floor : len / NO_RATE_MM_MIN;
        incomplete = true;
    }
    return minutes * 60.0 + m.dwellBefore;
}

SegmentList::Estimate SegmentList::estimate(const Rates &rates) {
    Estimate e = {0, false};
    Reader reader(*this);
    Move m;
    uint32_t nextCp = 0;
    while (reader.next(m)) {
        while (nextCp < cpCount && nextCp * CHECKPOINT_EVERY <= m.record) {
            cps()[-(int32_t)nextCp - 1].seconds = (float)e.seconds;
            nextCp++;
        }
        e.seconds += moveSeconds(m, rates, e.incomplete);
    }
    while (nextCp < cpCount) {
        cps()[-(int32_t)nextCp - 1].seconds = (float)e.seconds;
        nextCp++;
    }
    e.seconds += reader.trailingDwell() + pendingDwell;
    return e;
}
