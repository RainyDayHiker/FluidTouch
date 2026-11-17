#include "app/screens/screens.h"
#include "ui/ui_theme.h"

void makePlaceholder(lv_obj_t *parent, const char *title, const char *text) {
    lv_obj_t *title_label = lv_label_create(parent);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title_label, UITheme::TEXT_LIGHT, 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *text_label = lv_label_create(parent);
    lv_label_set_text(text_label, text);
    lv_obj_set_style_text_font(text_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(text_label, UITheme::TEXT_MEDIUM, 0);
    lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(text_label, LV_ALIGN_CENTER, 0, 10);
}
