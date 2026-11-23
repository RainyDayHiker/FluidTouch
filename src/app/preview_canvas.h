#pragma once

#include <lvgl.h>
#include <stdint.h>

namespace PreviewDraw {
struct Target;
struct Palette;
}

// The chosen job drawn from its segment list (specs/11-preview.md §4, D28): an RGB565 canvas
// drawn by our own rasteriser, as spike S2 measured. Top view, Y up. Fit shows the job's extents
// (with the stock box); a double tap zooms 2x on the tapped point, up to 16x; when zoomed in, a
// drag moves the view (D85).
//
// Draws on the main loop; the reference job takes about 0.2 s on the device. While a finger
// drags, the picture already drawn moves with it, and it is drawn again when the finger lifts.
//
// Run mode (§6, D89): the job as on Job and the moves before done() in one bright colour over
// it, added as the tool moves on; a marker at the tool, which the view follows when zoomed in.
class PreviewCanvas {
public:
    // False when the canvas buffer can't be had (the area stays empty)
    bool create(lv_obj_t *parent, int x, int y, int w, int h, bool runMode = false);
    // The preview's box (the canvas inside it), to show or hide
    lv_obj_t *object() const { return box; }

    // Draws PreviewService's job at the current view; an empty canvas when it has none
    void draw();
    // Back to the whole job
    void fit();
    // Zooms in 2x centred on a point of the canvas (pixels from its top left); at 16x it centres
    void zoomAt(int px, int py);
    int zoom() const { return zoomLevel; }
    // The time the last draw() took, ms
    uint32_t lastDrawMs() const { return drawMs; }

    // Run mode: the moves before `record` are done. Draws only what changed; going back redraws
    void setDone(uint32_t record);
    // Run mode: a new job or run: Fit, with the moves before `record` done, drawn once
    void restart(uint32_t record);
    uint32_t done() const { return doneRecord; }
    // Run mode: the tool's work position (mm), or hidden. Zoomed in, the view follows it
    void setMarker(bool visible, float x = 0, float y = 0);

private:
    struct View {
        float scale;   // pixels per mm
        float ox, oy;  // mm at the canvas's bottom left pixel
    };
    void computeFit();
    // Moves the view by a drag of (dx, dy) pixels
    void pan(int dx, int dy);
    // Moves the view so some of what Fit shows stays on the canvas
    void keepInView();
    // The moves from record `from` up to `to` (exclusive), in the palette
    void drawMoves(const PreviewDraw::Target &t, uint32_t from, uint32_t to, const PreviewDraw::Palette &p);
    void drawOverlays(const PreviewDraw::Target &t);
    void placeMarker();
    static void onEvent(lv_event_t *e);

    lv_obj_t *box = nullptr;     // clips the canvas while it is dragged
    lv_obj_t *canvas = nullptr;
    lv_obj_t *marker = nullptr;  // run mode: the tool, over the canvas
    void *buffer = nullptr;
    int width = 0, height = 0;
    bool run = false;
    View view = {1, 0, 0};
    float fitScale = 1;
    float fitMinX = 0, fitMaxX = 0, fitMinY = 0, fitMaxY = 0;  // what Fit shows, mm
    int zoomLevel = 1;
    uint32_t drawMs = 0;
    // Run mode
    uint32_t doneRecord = 0;
    bool markerShown = false;
    float markerX = 0, markerY = 0;
    bool follow = true;  // a drag stops following; Fit and a double tap start it again
    // A drag: where the finger went down, and whether it has moved far enough to be one
    lv_point_t pressAt = {0, 0};
    bool dragging = false;
    // The last tap, for a double tap
    bool haveTap = false;
    lv_point_t tapAt = {0, 0};
    uint32_t tapMs = 0;
};
