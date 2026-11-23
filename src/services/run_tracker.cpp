#include "services/run_tracker.h"
#include <math.h>

namespace {

const float TWO_PI = 6.28318530718f;

}  // namespace

void RunTracker::reset() {
    have = false;
    rec = 0;
    ln = 0;
}

uint32_t RunTracker::windowEnd(const SegmentList &list, uint64_t readOffset) {
    if (readOffset == READ_ALL) return list.records();
    // A checkpoint holds the offset of the move before it: from the first one past the read
    // position, nothing has been read
    for (uint32_t i = 1; i < list.checkpoints(); i++) {
        if (list.checkpoint(i).offset > readOffset) return i * SegmentList::CHECKPOINT_EVERY;
    }
    return list.records();
}

bool RunTracker::near(const SegmentList::Move &m, float x, float y, float z, float zTolerance) {
    // The point on the move nearest the position in XY, as a fraction t along it
    float t = 0, d2 = 0;
    if (m.kind == SegmentList::ARC_CW || m.kind == SegmentList::ARC_CCW) {
        float a0 = atan2f(m.y0 - m.cy, m.x0 - m.cx);
        float a = atan2f(y - m.cy, x - m.cx);
        float d = a - a0;  // the position's angle from the start, in the arc's direction
        if (m.sweep >= 0) {
            while (d < 0) d += TWO_PI;
            while (d >= TWO_PI) d -= TWO_PI;
        } else {
            while (d > 0) d -= TWO_PI;
            while (d <= -TWO_PI) d += TWO_PI;
        }
        if (m.sweep != 0 && d / m.sweep <= 1) {
            float r = hypotf(x - m.cx, y - m.cy) - m.r;
            d2 = r * r;
            t = d / m.sweep;
        } else {
            float s2 = (x - m.x0) * (x - m.x0) + (y - m.y0) * (y - m.y0);
            float e2 = (x - m.x1) * (x - m.x1) + (y - m.y1) * (y - m.y1);
            d2 = s2 < e2 ? s2 : e2;
            t = s2 < e2 ? 0 : 1;
        }
    } else {
        float dx = m.x1 - m.x0, dy = m.y1 - m.y0;
        float len2 = dx * dx + dy * dy;
        if (len2 > 0) {
            t = ((x - m.x0) * dx + (y - m.y0) * dy) / len2;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
        }
        float px = m.x0 + t * dx - x, py = m.y0 + t * dy - y;
        d2 = px * px + py * py;
    }
    if (d2 > XY_TOLERANCE * XY_TOLERANCE) return false;
    // Z at that point; a move straight down or up (no XY) passes every Z between its ends
    if (m.x0 == m.x1 && m.y0 == m.y1 && m.kind != SegmentList::ARC_CW && m.kind != SegmentList::ARC_CCW) {
        float lo = m.z0 < m.z1 ? m.z0 : m.z1, hi = m.z0 < m.z1 ? m.z1 : m.z0;
        return z >= lo - zTolerance && z <= hi + zTolerance;
    }
    float mz = m.z0 + t * (m.z1 - m.z0);
    return fabsf(mz - z) <= zTolerance;
}

bool RunTracker::update(const SegmentList &list, float x, float y, float z, uint64_t readOffset) {
    if (!list.records()) return false;
    const uint32_t from = have ? rec : 0;
    const uint32_t end = windowEnd(list, readOffset);
    const float zTolerance = list.zStep() > Z_TOLERANCE ? list.zStep() : Z_TOLERANCE;
    SegmentList::Reader reader(list, from / SegmentList::CHECKPOINT_EVERY);
    SegmentList::Move m;
    while (reader.next(m)) {
        if (m.record < from) continue;
        if (m.record >= end) break;
        if (near(m, x, y, z, zTolerance)) {
            bool moved = !have || m.record != rec;
            have = true;
            rec = m.record;
            ln = m.line;
            return moved;
        }
    }
    return false;
}
