#include "app/app.h"
#include "app/ui_mode.h"
#include <Arduino.h>
#include <lvgl.h>
#include "core/display_driver.h"
#include "core/power_manager.h"
#include "network/fluidnc_client.h"
#include "network/screenshot_server.h"
#include "ui/ui_theme.h"

bool LrtApp::running = false;

static void on_restart_classic(lv_event_t *e) {
    UiMode::restartInto(UiMode::CLASSIC);
}

void LrtApp::begin(DisplayDriver *display) {
    Serial.println("LrtApp: starting LowRider Touch");

    lv_obj_t *screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, UITheme::BG_DARKER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "LowRider Touch");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, UITheme::TEXT_LIGHT, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    lv_obj_t *btn = lv_btn_create(screen);
    lv_obj_set_size(btn, 320, 64);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(btn, UITheme::BG_BUTTON, 0);
    lv_obj_add_event_cb(btn, on_restart_classic, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Restart into classic UI");
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_20, 0);
    lv_obj_center(btn_label);

    lv_scr_load(screen);
    running = true;
}

void LrtApp::loop() {
    ScreenshotServer::handleClient();
    FluidNCClient::loop();

    uint32_t now = millis();

    static uint32_t lastSlow = 0;
    if (now - lastSlow >= 250) {
        lastSlow = now;
        PowerManager::update(STATE_IDLE);
    }

    static uint32_t lastTick = 0;
    lv_tick_inc(now - lastTick);
    lastTick = now;

    lv_timer_handler();
    delay(5);
}
