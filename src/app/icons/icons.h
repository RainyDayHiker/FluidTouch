#ifndef LRT_ICONS_H
#define LRT_ICONS_H

#include <lvgl.h>

// The new UI's own icons, alpha only (A8): they take the colour set with
// Widgets::addIcon. Drawn by make_icons.py, which writes icons.cpp.
extern const lv_image_dsc_t ICON_ZERO;       // slashed zero, 18 x 22
extern const lv_image_dsc_t ICON_CROSSHAIR;  // laser crosshair, 24 x 24
extern const lv_image_dsc_t ICON_BIT;        // end mill, 12 x 24
extern const lv_image_dsc_t ICON_JOYSTICK;   // joystick, 28 x 28
extern const lv_image_dsc_t ICON_ARROWS;     // four arrowheads, 28 x 28

#endif // LRT_ICONS_H
