#include "app/screens/draw_test.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include "ui/ui_theme.h"

// Spike S2. A synthetic job with the size and mix of the reference job
// (Spoilboard_DogHoles: 14,189 moves, 10,689 of them arcs) is held as a
// segment list in PSRAM and drawn to a 480 x 360 RGB565 canvas, whole and at
// 5x zoom. Two ways of drawing are timed: writing pixels straight into the
// canvas buffer, and LVGL's draw functions.

namespace {

const int CANVAS_W = 480;
const int CANVAS_H = 360;
const int TOTAL_SEGMENTS = 14189;
const int ARC_SEGMENTS = 10689;

// Job area in mm (a LowRider-sized sheet)
const float JOB_W = 1200.0f;
const float JOB_H = 600.0f;

enum Kind : uint8_t { RAPID, LINE, ARC_CW, ARC_CCW };

// 24 bytes, the size budgeted in specs/02-architecture.md section 6. Arcs
// stay arcs: end points plus radius, flattened only while drawing.
struct Segment {
    float x0, y0, x1, y1;
    float r;        // arcs only
    uint8_t kind;
    uint8_t level;  // depth level, picks the colour
    uint16_t pad;
};

struct View {
    float scale;   // pixels per mm
    float ox, oy;  // job coordinates of the canvas's bottom-left corner
};

Segment *segments = nullptr;
int segmentCount = 0;
void *baseBuf = nullptr;      // canvas buffer: the whole job
void *progressBuf = nullptr;  // second layer, as the Run screen would need
size_t canvasBytes = 0;

lv_obj_t *root = nullptr;
lv_obj_t *canvas = nullptr;
lv_obj_t *resultLabel = nullptr;
bool zoomed = false;
char results[900];

uint16_t levelColors[8];
uint16_t rapidColor;
uint16_t backgroundColor;

void *psAlloc(size_t bytes) {
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// --- Synthetic job -----------------------------------------------------------

void addSegment(uint8_t kind, uint8_t level, float x0, float y0, float x1, float y1, float r) {
    if (segmentCount >= TOTAL_SEGMENTS) return;
    Segment &s = segments[segmentCount++];
    s.x0 = x0; s.y0 = y0; s.x1 = x1; s.y1 = y1;
    s.r = r; s.kind = kind; s.level = level; s.pad = 0;
}

// 50 pockets, each cleared with concentric half circles at six depths, joined
// by rapids; short radial cuts fill up to the reference count.
void generateJob() {
    segmentCount = 0;
    int arcs = 0;
    float px = 0, py = 0;
    for (int hole = 0; hole < 50 && arcs < ARC_SEGMENTS; hole++) {
        float cx = 60.0f + (hole % 10) * 120.0f;
        float cy = 60.0f + (hole / 10) * 120.0f;
        addSegment(RAPID, 0, px, py, cx, cy, 0);
        px = cx; py = cy;
        for (int level = 0; level < 6 && arcs < ARC_SEGMENTS; level++) {
            for (int ring = 0; ring < 18 && arcs < ARC_SEGMENTS; ring++) {
                float r = 1.0f + ring * 0.5f;
                if (ring % 2 == 0) {
                    addSegment(LINE, level, px, py, cx + r, cy, 0);  // step over
                }
                addSegment(ARC_CCW, level, cx + r, cy, cx - r, cy, r);
                addSegment(ARC_CCW, level, cx - r, cy, cx + r, cy, r);
                arcs += 2;
                px = cx + r; py = cy;
            }
        }
    }
    // Short radial cuts around the pockets fill up to the reference count
    int extra = 0;
    while (segmentCount < TOTAL_SEGMENTS) {
        int hole = extra % 50;
        float cx = 60.0f + (hole % 10) * 120.0f;
        float cy = 60.0f + (hole / 10) * 120.0f;
        float angle = (extra / 50) * 0.42f;
        float c = cosf(angle), sn = sinf(angle);
        addSegment(LINE, 6, cx + 12.0f * c, cy + 12.0f * sn, cx + 20.0f * c, cy + 20.0f * sn, 0);
        extra++;
    }
}

// --- Drawing straight into the buffer ----------------------------------------

struct Target {
    uint16_t *pixels;
    int stride;  // in pixels
};

inline void plot(const Target &t, int x, int y, uint16_t color) {
    t.pixels[y * t.stride + x] = color;
}

// Line in pixel coordinates, clipped to the canvas (Liang-Barsky), then DDA
void drawLinePx(const Target &t, float x0, float y0, float x1, float y1, uint16_t color) {
    float dx = x1 - x0, dy = y1 - y0;
    float t0 = 0.0f, t1 = 1.0f;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {x0, (CANVAS_W - 1) - x0, y0, (CANVAS_H - 1) - y0};
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return;
        } else {
            float r = q[i] / p[i];
            if (p[i] < 0.0f) {
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
        int x = (int)(ax + 0.5f), y = (int)(ay + 0.5f);
        if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H) plot(t, x, y, color);
        ax += sx; ay += sy;
    }
}

inline float toPxX(const View &v, float x) { return (x - v.ox) * v.scale; }
inline float toPxY(const View &v, float y) { return (CANVAS_H - 1) - (y - v.oy) * v.scale; }

// Centre of an arc given by its end points and radius (the minor arc)
bool arcCentre(const Segment &s, float &cx, float &cy) {
    float mx = (s.x0 + s.x1) * 0.5f, my = (s.y0 + s.y1) * 0.5f;
    float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
    float d = sqrtf(dx * dx + dy * dy);
    if (d <= 0.0f) return false;
    float h2 = s.r * s.r - d * d * 0.25f;
    float h = h2 > 0.0f ? sqrtf(h2) : 0.0f;
    float sign = s.kind == ARC_CCW ? 1.0f : -1.0f;
    cx = mx - sign * h * dy / d;
    cy = my + sign * h * dx / d;
    return true;
}

void drawSegmentDirect(const Target &t, const View &v, const Segment &s) {
    uint16_t color = s.kind == RAPID ? rapidColor : levelColors[s.level & 7];
    if (s.kind == RAPID || s.kind == LINE) {
        drawLinePx(t, toPxX(v, s.x0), toPxY(v, s.y0), toPxX(v, s.x1), toPxY(v, s.y1), color);
        return;
    }
    float cx, cy;
    if (!arcCentre(s, cx, cy)) return;
    float rPx = s.r * v.scale;
    float pcx = toPxX(v, cx), pcy = toPxY(v, cy);
    if (pcx + rPx < 0 || pcx - rPx > CANVAS_W || pcy + rPx < 0 || pcy - rPx > CANVAS_H) return;

    float a0 = atan2f(s.y0 - cy, s.x0 - cx);
    float a1 = atan2f(s.y1 - cy, s.x1 - cx);
    float sweep = a1 - a0;
    if (s.kind == ARC_CCW) {
        if (sweep <= 0.0f) sweep += 2.0f * (float)M_PI;
    } else {
        if (sweep >= 0.0f) sweep -= 2.0f * (float)M_PI;
    }
    // Chords that stay within a quarter pixel of the true arc
    int n = 1;
    if (rPx > 0.5f) {
        float step = 2.0f * acosf(1.0f - 0.25f / rPx);
        n = (int)ceilf(fabsf(sweep) / step);
        if (n < 1) n = 1;
    }
    float da = sweep / n;
    float cosd = cosf(da), sind = sinf(da);
    float vx = s.x0 - cx, vy = s.y0 - cy;
    float lastX = toPxX(v, s.x0), lastY = toPxY(v, s.y0);
    for (int i = 0; i < n; i++) {
        float nx = vx * cosd - vy * sind;
        float ny = vx * sind + vy * cosd;
        vx = nx; vy = ny;
        float x = toPxX(v, cx + vx), y = toPxY(v, cy + vy);
        drawLinePx(t, lastX, lastY, x, y, color);
        lastX = x; lastY = y;
    }
}

Target canvasTarget() {
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(canvas);
    return {(uint16_t *)buf->data, (int)(buf->header.stride / 2)};
}

void clearTarget(const Target &t) {
    for (int y = 0; y < CANVAS_H; y++) {
        uint16_t *row = t.pixels + y * t.stride;
        for (int x = 0; x < CANVAS_W; x++) row[x] = backgroundColor;
    }
}

View fullView() {
    return {CANVAS_W / JOB_W, 0.0f, (JOB_H - CANVAS_H / (CANVAS_W / JOB_W)) * 0.5f};
}

// 5x: centred on a pocket near the middle of the sheet
View zoomView() {
    float scale = 5.0f * CANVAS_W / JOB_W;
    return {scale, 540.0f - (CANVAS_W / scale) * 0.5f, 300.0f - (CANVAS_H / scale) * 0.5f};
}

uint32_t drawDirect(const View &v) {
    Target t = canvasTarget();
    uint32_t start = micros();
    clearTarget(t);
    for (int i = 0; i < segmentCount; i++) drawSegmentDirect(t, v, segments[i]);
    return micros() - start;
}

// --- Drawing with LVGL's draw functions --------------------------------------

uint32_t drawWithLvgl(const View &v) {
    uint32_t start = micros();
    lv_canvas_fill_bg(canvas, lv_color_hex(0x101010), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    int queued = 0;
    for (int i = 0; i < segmentCount; i++) {
        const Segment &s = segments[i];
        lv_color_t color = s.kind == RAPID ? lv_color_hex(0x505050) : lv_palette_main(LV_PALETTE_LIGHT_BLUE);
        if (s.kind == RAPID || s.kind == LINE) {
            lv_draw_line_dsc_t dsc;
            lv_draw_line_dsc_init(&dsc);
            dsc.color = color;
            dsc.width = 1;
            dsc.p1.x = (int32_t)toPxX(v, s.x0);
            dsc.p1.y = (int32_t)toPxY(v, s.y0);
            dsc.p2.x = (int32_t)toPxX(v, s.x1);
            dsc.p2.y = (int32_t)toPxY(v, s.y1);
            lv_draw_line(&layer, &dsc);
        } else {
            float cx, cy;
            if (!arcCentre(s, cx, cy)) continue;
            float rPx = s.r * v.scale;
            float pcx = toPxX(v, cx), pcy = toPxY(v, cy);
            if (pcx + rPx < 0 || pcx - rPx > CANVAS_W || pcy + rPx < 0 || pcy - rPx > CANVAS_H) continue;
            // LVGL angles: degrees, clockwise on screen, y pointing down
            float a0 = -atan2f(s.y0 - cy, s.x0 - cx) * 180.0f / (float)M_PI;
            float a1 = -atan2f(s.y1 - cy, s.x1 - cx) * 180.0f / (float)M_PI;
            if (s.kind == ARC_CCW) { float tmp = a0; a0 = a1; a1 = tmp; }
            if (a0 < 0) a0 += 360.0f;
            if (a1 < 0) a1 += 360.0f;
            lv_draw_arc_dsc_t dsc;
            lv_draw_arc_dsc_init(&dsc);
            dsc.color = color;
            dsc.width = 1;
            dsc.center.x = (int32_t)pcx;
            dsc.center.y = (int32_t)pcy;
            dsc.radius = (uint16_t)(rPx < 1.0f ? 1 : rPx);
            dsc.start_angle = (int32_t)a0;
            dsc.end_angle = (int32_t)a1;
            lv_draw_arc(&layer, &dsc);
        }
        // Finish in batches: every queued task takes LVGL memory until drawn
        if (++queued >= 64) {
            lv_canvas_finish_layer(canvas, &layer);
            lv_canvas_init_layer(canvas, &layer);
            queued = 0;
        }
    }
    lv_canvas_finish_layer(canvas, &layer);
    return micros() - start;
}

// --- Screen ------------------------------------------------------------------

uint32_t showCanvas() {
    uint32_t start = micros();
    lv_obj_invalidate(canvas);
    lv_refr_now(nullptr);
    return micros() - start;
}

void setResults(const char *text) {
    lv_label_set_text(resultLabel, text);
    Serial.println("--- S2 draw test ---");
    Serial.println(text);
}

void freeBuffers() {
    if (segments) heap_caps_free(segments);
    if (baseBuf) heap_caps_free(baseBuf);
    if (progressBuf) heap_caps_free(progressBuf);
    segments = nullptr;
    baseBuf = nullptr;
    progressBuf = nullptr;
}

void runTest() {
    lv_label_set_text(resultLabel, "Running...");
    lv_refr_now(nullptr);

    uint32_t t0 = micros();
    generateJob();
    uint32_t genUs = micros() - t0;

    uint32_t fullUs = drawDirect(fullView());
    uint32_t zoomUs = drawDirect(zoomView());

    // The Run screen's progress layer: copy the finished base picture, then
    // redraw the cut part on top. Worst case the cut part is the whole job.
    uint32_t copyUs = 0;
    if (progressBuf) {
        t0 = micros();
        memcpy(progressBuf, baseBuf, canvasBytes);
        copyUs = micros() - t0;
    }

    zoomed = false;
    drawDirect(fullView());
    uint32_t showUs = showCanvas();

    snprintf(results, sizeof(results),
             "S2 draw test (synthetic job)\n"
             "Segments: %d (%d arcs)\n"
             "Segment list: %u KB\n"
             "Canvas: 2 x %u KB %s\n"
             "\n"
             "Free heap: %u KB\n"
             "Free PSRAM: %u KB\n"
             "Largest PSRAM block: %u KB\n"
             "(free values are with the\n"
             "buffers above allocated)\n"
             "\n"
             "Generate: %u ms\n"
             "Draw full: %u ms\n"
             "Draw 5x: %u ms\n"
             "Copy layer: %u.%u ms\n"
             "Show on screen: %u ms",
             segmentCount, ARC_SEGMENTS,
             (unsigned)(TOTAL_SEGMENTS * sizeof(Segment) / 1024),
             (unsigned)(canvasBytes / 1024), progressBuf ? "" : "(2nd FAILED)",
             (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(ESP.getFreePsram() / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(genUs / 1000), (unsigned)(fullUs / 1000), (unsigned)(zoomUs / 1000),
             (unsigned)(copyUs / 1000), (unsigned)((copyUs % 1000) / 100),
             (unsigned)(showUs / 1000));
    setResults(results);
}

void onRun(lv_event_t *e) {
    if (!canvas) return;
    runTest();
}

void onLvgl(lv_event_t *e) {
    if (!canvas) return;
    lv_label_set_text(resultLabel, "Running LVGL draw...\n(can take a while)");
    lv_refr_now(nullptr);
    uint32_t fullUs = drawWithLvgl(fullView());
    uint32_t zoomUs = drawWithLvgl(zoomView());
    zoomed = true;
    showCanvas();
    char text[160];
    snprintf(text, sizeof(text),
             "LVGL draw functions\n(lv_draw_line, lv_draw_arc)\n\nDraw full: %u ms\nDraw 5x: %u ms",
             (unsigned)(fullUs / 1000), (unsigned)(zoomUs / 1000));
    setResults(text);
}

void onZoom(lv_event_t *e) {
    if (!canvas) return;
    zoomed = !zoomed;
    drawDirect(zoomed ? zoomView() : fullView());
    lv_obj_invalidate(canvas);
}

void onBack(lv_event_t *e) {
    lv_obj_delete(root);
    root = nullptr;
    canvas = nullptr;
    resultLabel = nullptr;
    freeBuffers();
}

lv_obj_t *button(lv_obj_t *parent, int x, const char *text, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 68, 56);
    lv_obj_set_pos(btn, x, 312);
    lv_obj_set_style_bg_color(btn, UITheme::BG_BUTTON, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_center(label);
    return btn;
}

}  // namespace

void DrawTest::open(lv_obj_t *parent) {
    if (root) return;

    root = lv_obj_create(parent);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, 800, 376);
    lv_obj_set_style_bg_color(root, UITheme::BG_DARKER, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    resultLabel = lv_label_create(root);
    lv_obj_set_pos(resultLabel, 500, 6);
    lv_obj_set_width(resultLabel, 292);
    lv_obj_set_style_text_font(resultLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(resultLabel, UITheme::TEXT_LIGHT, 0);
    lv_label_set_text(resultLabel, "");

    button(root, 500, "Run", onRun);
    button(root, 575, "LVGL", onLvgl);
    button(root, 650, "Zoom", onZoom);
    button(root, 725, "Back", onBack);

    // Depth levels from light to dark blue, radial cuts in green
    static const uint32_t levels[8] = {0x9BD7FF, 0x6CC0FF, 0x3FA6F5, 0x1E88E0, 0x0F6BC0, 0x08509A, 0x3FBF6F, 0xFFFFFF};
    for (int i = 0; i < 8; i++) levelColors[i] = lv_color_to_u16(lv_color_hex(levels[i]));
    rapidColor = lv_color_to_u16(lv_color_hex(0x505050));
    backgroundColor = lv_color_to_u16(lv_color_hex(0x101010));

    canvasBytes = LV_CANVAS_BUF_SIZE(CANVAS_W, CANVAS_H, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    segments = (Segment *)psAlloc(TOTAL_SEGMENTS * sizeof(Segment));
    baseBuf = psAlloc(canvasBytes);
    progressBuf = psAlloc(canvasBytes);
    if (!segments || !baseBuf) {
        snprintf(results, sizeof(results),
                 "Allocation FAILED\nsegment list %s\ncanvas %s\n\nFree PSRAM: %u KB\nLargest block: %u KB",
                 segments ? "ok" : "failed", baseBuf ? "ok" : "failed",
                 (unsigned)(ESP.getFreePsram() / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        setResults(results);
        freeBuffers();
        return;
    }

    canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(canvas, baseBuf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas, 8, 8);

    runTest();
}
