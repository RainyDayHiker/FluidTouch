#include "app/screens/screens.h"
#include "app/ui_mode.h"
#include "app/screens/draw_test.h"
#include "ui/ui_theme.h"

static void on_restart_classic(lv_event_t *e) {
    UiMode::restartInto(UiMode::CLASSIC);
}

static void on_draw_test(lv_event_t *e) {
    DrawTest::open((lv_obj_t *)lv_event_get_user_data(e));
}

// Placeholder until M3 (specs/03-screens.md section 6). "Restart into classic
// UI" belongs to the Tools page; it is here already because the classic UI is
// the fallback for everything the new screens don't do yet (G10).
void SettingsScreen::create(lv_obj_t *parent) {
    makePlaceholder(parent, "Settings", "Settings pages. Not built yet (M3).");

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 320, 64);
    lv_obj_align(btn, LV_ALIGN_CENTER, -170, 100);
    lv_obj_set_style_bg_color(btn, UITheme::BG_BUTTON, 0);
    lv_obj_add_event_cb(btn, on_restart_classic, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, "Restart into classic UI");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_center(label);

    // Spike S2 (specs/06-test-and-spikes.md); goes away with the real preview
    lv_obj_t *test_btn = lv_btn_create(parent);
    lv_obj_set_size(test_btn, 320, 64);
    lv_obj_align(test_btn, LV_ALIGN_CENTER, 170, 100);
    lv_obj_set_style_bg_color(test_btn, UITheme::BG_BUTTON, 0);
    lv_obj_add_event_cb(test_btn, on_draw_test, LV_EVENT_CLICKED, parent);
    lv_obj_t *test_label = lv_label_create(test_btn);
    lv_label_set_text(test_label, "Draw test (spike S2)");
    lv_obj_set_style_text_font(test_label, &lv_font_montserrat_20, 0);
    lv_obj_center(test_label);
}
