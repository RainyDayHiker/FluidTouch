#include "app/widgets.h"
#include "ui/ui_theme.h"
#include <string.h>

namespace Widgets {

void setText(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *text, const lv_font_t *font,
                 lv_event_cb_t onClick, void *userData) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 2, 0);
    lv_obj_set_style_bg_color(btn, UITheme::BG_BUTTON, 0);
    lv_obj_set_style_text_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_color(btn, UITheme::BG_MEDIUM, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(btn, UITheme::TEXT_DISABLED, LV_STATE_DISABLED);
    if (onClick) lv_obj_add_event_cb(btn, onClick, LV_EVENT_CLICKED, userData);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    return btn;
}

void setEnabled(lv_obj_t *btn, bool enabled) {
    if (lv_obj_has_state(btn, LV_STATE_DISABLED) != enabled) return;
    if (enabled) {
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

void setButtonColor(lv_obj_t *btn, lv_color_t color) {
    // The local value, not the drawn one, which differs while disabled
    lv_style_value_t current;
    if (lv_obj_get_local_style_prop(btn, LV_STYLE_BG_COLOR, &current, LV_PART_MAIN | LV_STATE_DEFAULT) ==
            LV_STYLE_RES_FOUND &&
        lv_color_eq(current.color, color)) {
        return;
    }
    lv_obj_set_style_bg_color(btn, color, 0);
}

lv_obj_t *label(lv_obj_t *parent, int x, int y, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

}  // namespace Widgets
