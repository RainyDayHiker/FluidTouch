#ifndef LRT_WIDGETS_H
#define LRT_WIDGETS_H

#include <lvgl.h>

// Small helpers shared by the frame and the screens
namespace Widgets {

// lv_label_set_text() redraws even when nothing changed
void setText(lv_obj_t *label, const char *text);

// A button with a centred label, at a fixed position. Disabled buttons are
// drawn dim. The label is the button's first child.
lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, const lv_font_t *font,
                 lv_event_cb_t onClick, void *userData = nullptr);

// Enables or disables a button; does nothing when it is already so
void setEnabled(lv_obj_t *btn, bool enabled);

// Sets a button's background (normal state); does nothing when unchanged
void setButtonColor(lv_obj_t *btn, lv_color_t color);

// A label at a fixed position
lv_obj_t *label(lv_obj_t *parent, int x, int y, const lv_font_t *font, lv_color_t color, const char *text);

}  // namespace Widgets

#endif // LRT_WIDGETS_H
