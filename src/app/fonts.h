#pragma once
#include <lvgl.h>

// The fonts of the new UI: LVGL's Montserrat (ASCII and a few symbols) with a fallback font of
// the same face for Latin-1, dashes, curly quotes and the ellipsis (app/fonts/, specs/04 §3,
// D71). Use these instead of lv_font_montserrat_<size> in src/app.
extern lv_font_t lrt_font_14;
extern lv_font_t lrt_font_16;
extern lv_font_t lrt_font_18;
extern lv_font_t lrt_font_20;
extern lv_font_t lrt_font_22;
extern lv_font_t lrt_font_24;
extern lv_font_t lrt_font_26;
extern lv_font_t lrt_font_32;
