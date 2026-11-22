#pragma once

#include <lvgl.h>
#include <stdint.h>

// The chosen job drawn from its segment list (specs/11-preview.md §4, D28): an RGB565 canvas
// drawn by our own rasteriser, as spike S2 measured. Top view, Y up. Fit shows the job's extents
// (with the stock box); a tap zooms 2x on the tapped point, up to 16x.
//
// Draws on the main loop; the reference job takes about 0.3 s on the device (S2).
class PreviewCanvas {
public:
    // False when the canvas buffer can't be had (the area stays empty)
    bool create(lv_obj_t *parent, int x, int y, int w, int h);
    lv_obj_t *object() const { return canvas; }

    // Draws PreviewService's job at the current view; an empty canvas when it has none
    void draw();
    // Back to the whole job
    void fit();
    // Zooms in 2x centred on a point of the canvas (pixels from its top left)
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
    static void onClicked(lv_event_t *e);

    lv_obj_t *canvas = nullptr;
    void *buffer = nullptr;
    int width = 0, height = 0;
    View view = {1, 0, 0};
    float fitScale = 1;
    int zoomLevel = 1;
    uint32_t drawMs = 0;
};
