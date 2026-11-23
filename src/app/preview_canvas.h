#pragma once

#include <lvgl.h>
#include <stdint.h>

// The chosen job drawn from its segment list (specs/11-preview.md §4, D28): an RGB565 canvas
// drawn by our own rasteriser, as spike S2 measured. Top view, Y up. Fit shows the job's extents
// (with the stock box); a double tap zooms 2x on the tapped point, up to 16x; when zoomed in, a
// drag moves the view (D85).
//
// Draws on the main loop; the reference job takes about 0.2 s on the device. While a finger
// drags, the picture already drawn moves with it, and it is drawn again when the finger lifts.
class PreviewCanvas {
public:
    // False when the canvas buffer can't be had (the area stays empty)
    bool create(lv_obj_t *parent, int x, int y, int w, int h);
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
    static void onEvent(lv_event_t *e);

    lv_obj_t *box = nullptr;     // clips the canvas while it is dragged
    lv_obj_t *canvas = nullptr;
    void *buffer = nullptr;
    int width = 0, height = 0;
    View view = {1, 0, 0};
    float fitScale = 1;
    float fitMinX = 0, fitMaxX = 0, fitMinY = 0, fitMaxY = 0;  // what Fit shows, mm
    int zoomLevel = 1;
    uint32_t drawMs = 0;
    // A drag: where the finger went down, and whether it has moved far enough to be one
    lv_point_t pressAt = {0, 0};
    bool dragging = false;
    // The last tap, for a double tap
    bool haveTap = false;
    lv_point_t tapAt = {0, 0};
    uint32_t tapMs = 0;
};
