#include "app/ui_mode.h"
#include "app/fonts.h"
#include "app/app_prefs.h"
#include <Preferences.h>
#include <lvgl.h>

namespace UiMode {

Mode get() {
    Preferences prefs;
    prefs.begin(LRT_PREFS_NAMESPACE, true);
    uint8_t stored = prefs.getUChar("ui_mode", APP);
    prefs.end();
    return stored == CLASSIC ? CLASSIC : APP;
}

void set(Mode mode) {
    Preferences prefs;
    prefs.begin(LRT_PREFS_NAMESPACE, false);
    prefs.putUChar("ui_mode", (uint8_t)mode);
    prefs.end();
}

void restartInto(Mode mode) {
    set(mode);
    Serial.printf("UiMode: restarting into %s UI\n", mode == CLASSIC ? "classic" : "new");

    lv_obj_t *label = lv_label_create(lv_layer_top());
    lv_label_set_text(label, "Restarting...");
    lv_obj_set_style_text_font(label, &lrt_font_32, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(label, 24, 0);
    lv_obj_center(label);
    lv_timer_handler();
    delay(500);

    ESP.restart();
}

}  // namespace UiMode
