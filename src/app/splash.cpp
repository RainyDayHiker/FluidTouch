#include "app/splash.h"
#include "app/fonts.h"
#include "app/version.h"
#include "config.h"
#include "ui/images/fluidnc_logo.h"
#include "ui/ui_theme.h"
#include <Arduino.h>
#include <lvgl.h>
#include <stdio.h>

namespace {

const uint32_t SHOW_MS = 2000;

lv_obj_t *splash = nullptr;
uint32_t shownAt = 0;

void hide() {
    if (!splash) return;
    lv_obj_delete(splash);
    splash = nullptr;
}

void onTap(lv_event_t *) { hide(); }

lv_obj_t *centred(lv_obj_t *parent, int y, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}

}  // namespace

void Splash::show() {
    // On the top layer, so the frame can be built underneath; it takes every touch meanwhile
    splash = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(splash, 0, 0);
    lv_obj_set_size(splash, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(splash, UITheme::BG_DARKER, 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash, 0, 0);
    lv_obj_set_style_radius(splash, 0, 0);
    lv_obj_clear_flag(splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(splash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(splash, onTap, LV_EVENT_CLICKED, nullptr);

    // The logo is 365 x 136
    lv_obj_t *logo = lv_image_create(splash);
    lv_image_set_src(logo, &fluidnc_logo);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -100);

    centred(splash, 12, &lrt_font_32, UITheme::UI_INFO, "LowRider Touch");
    centred(splash, 52, &lrt_font_18, UITheme::TEXT_LIGHT, "Touch controller for FluidNC");
    char buf[128];
    const char *detail = Version::detail();
    snprintf(buf, sizeof(buf), detail[0] ? "%s  (%s)" : "%s", Version::name(), detail);
    centred(splash, 100, &lrt_font_20, UITheme::TEXT_LIGHT, buf);
    snprintf(buf, sizeof(buf), "Built %s, on FluidTouch " FLUIDTOUCH_VERSION, Version::built());
    centred(splash, 132, &lrt_font_14, UITheme::TEXT_MEDIUM, buf);

    Serial.printf("Splash: LowRider Touch %s (%s), built %s\n", Version::name(), detail, Version::built());
    lv_refr_now(nullptr);
    shownAt = millis();
}

void Splash::poll() {
    if (splash && millis() - shownAt >= SHOW_MS) hide();
}
