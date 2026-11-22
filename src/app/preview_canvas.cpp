#include "app/preview_canvas.h"
#include "services/preview_service.h"
#include "services/segment_list.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>

// Drawing straight into the canvas buffer, as spike S2 chose over LVGL's draw functions
// (D28): lines by DDA after clipping, arcs as chords within a quarter pixel.

namespace {

// Colours (P4, D82): depth levels light to dark over the job's cut Z range, rapids grey
const uint32_t LEVELS[8] = {0xCDEBFF, 0x9BD7FF, 0x6CC0FF, 0x3FA6F5, 0x1E88E0, 0x1270C8, 0x0C5AA8, 0x084488};
const uint32_t RAPID = 0x5A5A5A;
const uint32_t BACKGROUND = 0x101010;
const uint32_t STOCK = 0xB08D57;
const uint32_t ZERO = 0xE8E8E8;
const uint32_t PROBE = 0xFB8C00;
const int MAX_ZOOM = 16;
const int MARGIN = 10;  // pixels around the job at Fit

uint16_t rgb(uint32_t hex) { return lv_color_to_u16(lv_color_hex(hex)); }

struct Target {
    uint16_t *pixels;
    int stride;  // pixels
    int w, h;
};

inline void plot(const Target &t, int x, int y, uint16_t c) {
    if (x >= 0 && x < t.w && y >= 0 && y < t.h) t.pixels[y * t.stride + x] = c;
}

inline void plot2(const Target &t, int x, int y, uint16_t c) {
    plot(t, x, y, c);
    plot(t, x + 1, y, c);
    plot(t, x, y + 1, c);
    plot(t, x + 1, y + 1, c);
}

// A line in pixel coordinates, clipped to the canvas (Liang-Barsky), 1 or 2 pixels wide; dashed
// when `dash` is set (on and off in pixels)
void line(const Target &t, float x0, float y0, float x1, float y1, uint16_t c, bool thick, int dash = 0) {
    float dx = x1 - x0, dy = y1 - y0;
    float t0 = 0, t1 = 1;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {x0 + 1, (t.w) - x0, y0 + 1, (t.h) - y0};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return;
        } else {
            float r = q[i] / p[i];
            if (p[i] < 0) {
                if (r > t1) return;
                if (r > t0) t0 = r;
            } else {
                if (r < t0) return;
                if (r < t1) t1 = r;
            }
        }
    }
    float ax = x0 + t0 * dx, ay = y0 + t0 * dy;
    float bx = x0 + t1 * dx, by = y0 + t1 * dy;
    float adx = fabsf(bx - ax), ady = fabsf(by - ay);
    int steps = (int)(adx > ady ? adx : ady) + 1;
    float sx = (bx - ax) / steps, sy = (by - ay) / steps;
    for (int i = 0; i <= steps; i++) {
        if (!dash || (i / dash) % 2 == 0) {
            int x = (int)floorf(ax + 0.5f), y = (int)floorf(ay + 0.5f);
            if (thick) {
                plot2(t, x, y, c);
            } else {
                plot(t, x, y, c);
            }
        }
        ax += sx;
        ay += sy;
    }
}

}  // namespace

bool PreviewCanvas::create(lv_obj_t *parent, int x, int y, int w, int h) {
    width = w;
    height = h;
    size_t bytes = LV_CANVAS_BUF_SIZE(w, h, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
        Serial.printf("PreviewCanvas: no memory for %d x %d (%lu bytes)\n", w, h, (unsigned long)bytes);
        return false;
    }
    canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, buffer, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas, x, y);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas, onClicked, LV_EVENT_CLICKED, this);
    lv_canvas_fill_bg(canvas, lv_color_hex(BACKGROUND), LV_OPA_COVER);
    return true;
}

void PreviewCanvas::onClicked(lv_event_t *e) {
    PreviewCanvas *self = (PreviewCanvas *)lv_event_get_user_data(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(self->canvas, &a);
    self->zoomAt(p.x - a.x1, p.y - a.y1);
    self->draw();
}

void PreviewCanvas::computeFit() {
    const PreviewService::Summary &s = PreviewService::summary();
    double minX = s.jobMinX, maxX = s.jobMaxX, minY = s.jobMinY, maxY = s.jobMaxY;
    if (s.hasStock) {
        minX = fmin(minX, s.stockMinX);
        maxX = fmax(maxX, s.stockMaxX);
        minY = fmin(minY, s.stockMinY);
        maxY = fmax(maxY, s.stockMaxY);
    }
    double w = maxX - minX, h = maxY - minY;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    double sx = (width - 2 * MARGIN) / w, sy = (height - 2 * MARGIN) / h;
    fitScale = (float)(sx < sy ? sx : sy);
    view.scale = fitScale;
    view.ox = (float)((minX + maxX) * 0.5 - (width * 0.5) / fitScale);
    view.oy = (float)((minY + maxY) * 0.5 - (height * 0.5) / fitScale);
}

void PreviewCanvas::fit() {
    zoomLevel = 1;
    if (PreviewService::summary().hasJob) computeFit();
}

void PreviewCanvas::zoomAt(int px, int py) {
    if (!PreviewService::segments()) return;
    float mx = view.ox + px / view.scale;
    float my = view.oy + (height - 1 - py) / view.scale;
    if (zoomLevel < MAX_ZOOM) zoomLevel *= 2;
    view.scale = fitScale * zoomLevel;
    view.ox = mx - (width * 0.5f) / view.scale;
    view.oy = my - (height * 0.5f) / view.scale;
}

void PreviewCanvas::draw() {
    if (!canvas) return;
    uint32_t start = millis();
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(canvas);
    Target t = {(uint16_t *)buf->data, (int)(buf->header.stride / 2), width, height};
    const uint16_t bg = rgb(BACKGROUND);
    for (int y = 0; y < height; y++) {
        uint16_t *row = t.pixels + y * t.stride;
        for (int x = 0; x < width; x++) row[x] = bg;
    }

    const SegmentList *list = PreviewService::segments();
    const PreviewService::Summary &s = PreviewService::summary();
    if (list && s.hasJob) {
        const View v = view;
        auto px = [&](float x) { return (x - v.ox) * v.scale; };
        auto py = [&](float y) { return (height - 1) - (y - v.oy) * v.scale; };

        uint16_t levels[8];
        for (int i = 0; i < 8; i++) levels[i] = rgb(LEVELS[i]);
        const uint16_t rapid = rgb(RAPID);
        const float top = list->hasCut() ? list->cutTop() : 0;
        const float range = list->hasCut() ? list->cutTop() - list->cutBottom() : 0;
        auto level = [&](float z) {
            if (range < 0.01f) return levels[4];
            int i = (int)((top - z) / range * 8);
            return levels[i < 0 ? 0 : i > 7 ? 7 : i];
        };

        // File order: a later pass is drawn over an earlier one
        SegmentList::Reader reader(*list);
        SegmentList::Move m;
        while (reader.next(m)) {
            if (m.kind == SegmentList::RAPID || m.fixed) {
                line(t, px(m.x0), py(m.y0), px(m.x1), py(m.y1), rapid, false);
                continue;
            }
            uint16_t c = level(m.z1);
            if (m.kind == SegmentList::FEED) {
                if (m.x0 == m.x1 && m.y0 == m.y1) {
                    // A plunge as a dot; a retract isn't drawn
                    if (m.z1 < m.z0) {
                        int x = (int)floorf(px(m.x1) + 0.5f), y = (int)floorf(py(m.y1) + 0.5f);
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++) plot(t, x + dx, y + dy, c);
                        }
                    }
                    continue;
                }
                line(t, px(m.x0), py(m.y0), px(m.x1), py(m.y1), c, true);
                continue;
            }
            // An arc, as chords within a quarter pixel of it
            float rPx = m.r * v.scale;
            float pcx = px(m.cx), pcy = py(m.cy);
            if (pcx + rPx < -2 || pcx - rPx > width + 2 || pcy + rPx < -2 || pcy - rPx > height + 2) continue;
            int n = 1;
            if (rPx > 0.5f) {
                float step = 2.0f * acosf(1.0f - 0.25f / rPx);
                n = (int)ceilf(fabsf(m.sweep) / step);
                if (n < 1) n = 1;
                if (n > 720) n = 720;
            }
            float da = m.sweep / n;
            float cosd = cosf(da), sind = sinf(da);
            float vx = m.x0 - m.cx, vy = m.y0 - m.cy;
            float lastX = px(m.x0), lastY = py(m.y0);
            for (int i = 0; i < n; i++) {
                float nx = vx * cosd - vy * sind;
                float ny = vx * sind + vy * cosd;
                vx = nx;
                vy = ny;
                float x = i == n - 1 ? px(m.x1) : px(m.cx + vx);
                float y = i == n - 1 ? py(m.y1) : py(m.cy + vy);
                line(t, lastX, lastY, x, y, c, true);
                lastX = x;
                lastY = y;
            }
        }

        // The stock box dashed, job zero as a cross, the probe point as a ring
        if (s.hasStock) {
            const uint16_t sc = rgb(STOCK);
            float x0 = px((float)s.stockMinX), x1 = px((float)s.stockMaxX);
            float y0 = py((float)s.stockMinY), y1 = py((float)s.stockMaxY);
            line(t, x0, y0, x1, y0, sc, false, 5);
            line(t, x1, y0, x1, y1, sc, false, 5);
            line(t, x1, y1, x0, y1, sc, false, 5);
            line(t, x0, y1, x0, y0, sc, false, 5);
        }
        const uint16_t zc = rgb(ZERO);
        float zx = px(0), zy = py(0);
        line(t, zx - 7, zy, zx + 7, zy, zc, false);
        line(t, zx, zy - 7, zx, zy + 7, zc, false);
        if (s.hasProbe) {
            const uint16_t pc = rgb(PROBE);
            float cx = px(s.probeX), cy = py(s.probeY);
            const int N = 24;
            for (int i = 0; i < N; i++) {
                float a0 = i * 2 * (float)M_PI / N, a1 = (i + 1) * 2 * (float)M_PI / N;
                line(t, cx + 6 * cosf(a0), cy + 6 * sinf(a0), cx + 6 * cosf(a1), cy + 6 * sinf(a1), pc, true);
            }
        }
    }
    lv_obj_invalidate(canvas);
    drawMs = millis() - start;
}
