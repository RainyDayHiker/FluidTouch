#include "app/frame.h"
#include "app/fonts.h"
#include "app/screens/screens.h"
#include "app/ui_mode.h"
#include "app/widgets.h"
#include "services/job_service.h"
#include "services/machine_service.h"
#include "ui/ui_theme.h"
#include <Arduino.h>

Frame::ScreenId Frame::currentId = Frame::SCREEN_SETUP;

namespace {

SetupScreen setupScreen;
JobScreen jobScreen;
RunScreen runScreen;
MacrosScreen macrosScreen;
SettingsScreen settingsScreen;
Screen *const screens[Frame::SCREEN_COUNT] = {&setupScreen, &jobScreen, &runScreen, &macrosScreen, &settingsScreen};

lv_obj_t *tabButtons[Frame::SCREEN_COUNT];
lv_obj_t *contents[Frame::SCREEN_COUNT];
lv_obj_t *runDot;  // on the Run tab while a job is active

// Status bar
lv_obj_t *statusBar;
lv_obj_t *stateLabel;
lv_obj_t *workLabels[3];
lv_obj_t *machineLabels[3];
lv_obj_t *wifiLabel;
lv_obj_t *messageLabel;
lv_obj_t *stopLabel;  // STOP, or ABORT during a job (D90)

// The message row shows FluidNC's last message, or for a while a notice of
// the display's own
const uint32_t NOTICE_MS = 5000;
char noticeText[96] = "";
uint32_t noticeAtMs = 0;
char lastDeviceMessage[128] = "";  // to tell when FluidNC sends a new one

// Banner over the status bar
enum BannerKind { BANNER_NONE, BANNER_ALARM, BANNER_LINK, BANNER_NO_MACHINE };
BannerKind bannerKind = BANNER_NONE;
lv_obj_t *banner;
lv_obj_t *bannerText;
lv_obj_t *bannerButton;
lv_obj_t *bannerButtonLabel;
bool bannerButtonEnabled = true;

const lv_color_t BANNER_ALARM_BG = LV_COLOR_MAKE(0x7A, 0x10, 0x10);
const lv_color_t BANNER_LINK_BG = LV_COLOR_MAKE(0x4A, 0x3A, 0x00);

using Widgets::setText;
using Widgets::setTextColor;

lv_obj_t *plainBox(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t bg) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, bg, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

lv_obj_t *textAt(lv_obj_t *parent, int x, const lv_font_t *font, lv_color_t color, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, x, 0);
    return label;
}

struct StateStyle {
    const char *text;
    lv_color_t color;
};

StateStyle stateStyle(const FluidNCStatus &s) {
    // FluidNC reports Idle during a G4 dwell inside a running file
    if (s.state == STATE_IDLE && s.is_sd_printing) return {"RUN", UITheme::STATE_RUN};
    switch (s.state) {
        case STATE_IDLE:  return {"IDLE", UITheme::STATE_IDLE};
        case STATE_RUN:   return {"RUN", UITheme::STATE_RUN};
        case STATE_HOLD:  return {"HOLD", UITheme::STATE_HOLD};
        case STATE_JOG:   return {"JOG", UITheme::STATE_RUN};
        case STATE_HOME:  return {"HOME", UITheme::STATE_RUN};
        case STATE_ALARM: return {"ALARM", UITheme::STATE_ALARM};
        case STATE_DOOR:  return {"DOOR", UITheme::STATE_HOLD};
        case STATE_CHECK: return {"CHECK", UITheme::STATE_UNKNOWN};
        case STATE_SLEEP: return {"SLEEP", UITheme::STATE_UNKNOWN};
        default:          return {"OFFLINE", UITheme::STATE_ALARM};
    }
}

}  // namespace

void Frame::create() {
    lv_obj_t *root = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(root, UITheme::BG_DARKER, LV_PART_MAIN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    createTabs(root);

    for (int i = 0; i < SCREEN_COUNT; i++) {
        contents[i] = plainBox(root, 0, TAB_HEIGHT, 800, CONTENT_HEIGHT, UITheme::BG_DARKER);
        screens[i]->create(contents[i]);
        lv_obj_add_flag(contents[i], LV_OBJ_FLAG_HIDDEN);
    }

    createStatusBar(root);
    createBanner(root);

    lv_scr_load(root);
    show(SCREEN_SETUP);
    update();
}

void Frame::createTabs(lv_obj_t *root) {
    const int tabWidth = 800 / SCREEN_COUNT;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        lv_obj_t *btn = lv_btn_create(root);
        lv_obj_set_pos(btn, i * tabWidth, 0);
        lv_obj_set_size(btn, tabWidth, TAB_HEIGHT);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, UITheme::BG_MEDIUM, 0);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(btn, 4, 0);
        lv_obj_set_style_border_color(btn, UITheme::BG_MEDIUM, 0);
        lv_obj_add_event_cb(btn, onTabClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, screens[i]->title());
        lv_obj_set_style_text_font(label, &lrt_font_20, 0);
        lv_obj_center(label);

        tabButtons[i] = btn;
    }

    // A job is active: a dot on the Run tab
    runDot = lv_obj_create(tabButtons[SCREEN_RUN]);
    lv_obj_set_size(runDot, 12, 12);
    lv_obj_set_style_radius(runDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(runDot, UITheme::STATE_RUN, 0);
    lv_obj_set_style_border_width(runDot, 0, 0);
    lv_obj_align(runDot, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_clear_flag(runDot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(runDot, LV_OBJ_FLAG_HIDDEN);
}

void Frame::createStatusBar(lv_obj_t *root) {
    statusBar = plainBox(root, 0, 480 - STATUS_HEIGHT, 800, STATUS_HEIGHT, UITheme::BG_DARK);
    lv_obj_set_style_border_side(statusBar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(statusBar, 1, 0);
    lv_obj_set_style_border_color(statusBar, UITheme::BORDER_MEDIUM, 0);

    stateLabel = textAt(statusBar, 10, &lrt_font_22, UITheme::STATE_UNKNOWN, "");

    // First row: work position, then machine position; fixed columns so
    // digits don't shift the rest
    const lv_font_t *font = &lrt_font_16;
    static const lv_color_t axisColors[3] = {UITheme::AXIS_X, UITheme::AXIS_Y, UITheme::AXIS_Z};
    Widgets::label(statusBar, 108, 8, font, UITheme::POS_WORK, "W");
    Widgets::label(statusBar, 384, 8, font, UITheme::POS_MACHINE, "M");
    for (int i = 0; i < 3; i++) {
        workLabels[i] = Widgets::label(statusBar, 130 + i * 84, 8, font, axisColors[i], "");
        machineLabels[i] = Widgets::label(statusBar, 406 + i * 84, 8, font, axisColors[i], "");
    }

    // Second row: the last message
    messageLabel = Widgets::label(statusBar, 108, 36, font, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(messageLabel, 540);
    lv_label_set_long_mode(messageLabel, LV_LABEL_LONG_DOT);

    wifiLabel = textAt(statusBar, 664, &lrt_font_18, UITheme::STATE_ALARM, LV_SYMBOL_WIFI);

    // STOP: feed hold, or jog cancel while jogging (S1); during a job it reads ABORT and
    // ends the job (D90). The bar is lower than the 56 px touch target, so the touch area
    // extends beyond the button.
    lv_obj_t *stop = lv_btn_create(statusBar);
    lv_obj_set_size(stop, 104, 52);
    lv_obj_align(stop, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(stop, UITheme::BTN_ESTOP, 0);
    lv_obj_set_style_shadow_width(stop, 0, 0);
    lv_obj_set_ext_click_area(stop, 10);
    lv_obj_add_event_cb(stop, onStopClicked, LV_EVENT_CLICKED, nullptr);
    stopLabel = lv_label_create(stop);
    lv_label_set_text(stopLabel, "STOP");
    lv_obj_set_style_text_font(stopLabel, &lrt_font_20, 0);
    lv_obj_center(stopLabel);
}

void Frame::createBanner(lv_obj_t *root) {
    banner = plainBox(root, 0, 480 - STATUS_HEIGHT, 800, STATUS_HEIGHT, BANNER_LINK_BG);

    bannerText = lv_label_create(banner);
    lv_label_set_text(bannerText, "");
    lv_obj_set_style_text_font(bannerText, &lrt_font_16, 0);
    lv_obj_set_style_text_color(bannerText, lv_color_white(), 0);
    lv_label_set_long_mode(bannerText, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(bannerText, 630);  // two lines of this font fit the bar
    lv_obj_set_style_text_line_space(bannerText, 0, 0);
    lv_obj_align(bannerText, LV_ALIGN_LEFT_MID, 10, 0);

    bannerButton = lv_btn_create(banner);
    lv_obj_set_size(bannerButton, 140, 52);
    lv_obj_align(bannerButton, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_shadow_width(bannerButton, 0, 0);
    lv_obj_set_ext_click_area(bannerButton, 10);
    lv_obj_add_event_cb(bannerButton, onBannerButtonClicked, LV_EVENT_CLICKED, nullptr);
    bannerButtonLabel = lv_label_create(bannerButton);
    lv_label_set_text(bannerButtonLabel, "");
    lv_obj_set_style_text_font(bannerButtonLabel, &lrt_font_18, 0);
    lv_obj_center(bannerButtonLabel);

    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
}

void Frame::show(ScreenId id) {
    static bool shownOnce = false;
    // Settings saves when it is left (D55)
    if (shownOnce && id != currentId) screens[currentId]->onHide();
    shownOnce = true;
    currentId = id;
    for (int i = 0; i < SCREEN_COUNT; i++) {
        bool selected = i == id;
        if (selected) {
            lv_obj_clear_flag(contents[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(contents[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(tabButtons[i], selected ? UITheme::BG_BUTTON : UITheme::BG_MEDIUM, 0);
        lv_obj_set_style_border_color(tabButtons[i], selected ? UITheme::ACCENT_PRIMARY : UITheme::BG_MEDIUM, 0);
        lv_obj_set_style_text_color(tabButtons[i], selected ? lv_color_white() : UITheme::TEXT_MEDIUM, 0);
    }
    screens[id]->onShow();
    // Drawn up to date at once, not on the next update
    screens[id]->update();
}

void Frame::update() {
    updateBanner();
    updateStatusBar();
    Widgets::setText(stopLabel, JobService::active() ? "ABORT" : "STOP");

    if (MachineService::jobActive()) {
        lv_obj_clear_flag(runDot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(runDot, LV_OBJ_FLAG_HIDDEN);
    }

    screens[currentId]->update();
}

void Frame::updateStatusBar() {
    if (!MachineService::online()) {
        setText(stateLabel, "OFFLINE");
        setTextColor(stateLabel, UITheme::STATE_ALARM);
        static const char *const dashes[3] = {"X ---.--", "Y ---.--", "Z ---.--"};
        for (int i = 0; i < 3; i++) {
            setText(workLabels[i], dashes[i]);
            setText(machineLabels[i], dashes[i]);
        }
        setTextColor(wifiLabel, MachineService::link() == MachineService::LINK_CONTROLLER ? UITheme::STATE_HOLD
                                                                                          : UITheme::STATE_ALARM);
        return;
    }

    const FluidNCStatus &s = MachineService::status();
    StateStyle style = stateStyle(s);
    setText(stateLabel, style.text);
    setTextColor(stateLabel, style.color);

    const float work[3] = {s.wpos_x, s.wpos_y, s.wpos_z};
    const float machine[3] = {s.mpos_x, s.mpos_y, s.mpos_z};
    static const char axis[3] = {'X', 'Y', 'Z'};
    char buf[24];
    for (int i = 0; i < 3; i++) {
        snprintf(buf, sizeof(buf), "%c %.2f", axis[i], work[i]);
        setText(workLabels[i], buf);
        snprintf(buf, sizeof(buf), "%c %.2f", axis[i], machine[i]);
        setText(machineLabels[i], buf);
    }
    setTextColor(wifiLabel, UITheme::STATE_IDLE);

    // A new message from FluidNC replaces a notice at once
    if (strcmp(s.last_message, lastDeviceMessage) != 0) {
        strncpy(lastDeviceMessage, s.last_message, sizeof(lastDeviceMessage) - 1);
        noticeText[0] = '\0';
    }
    static bool noticeShown = false;
    bool showNotice = noticeText[0] && millis() - noticeAtMs < NOTICE_MS;
    if (!showNotice) noticeText[0] = '\0';
    setText(messageLabel, showNotice ? noticeText : s.last_message);
    if (showNotice != noticeShown) {
        noticeShown = showNotice;
        lv_obj_set_style_text_color(messageLabel, showNotice ? UITheme::UI_NOTE : UITheme::TEXT_LIGHT, 0);
    }
}

void Frame::notice(const char *text) {
    strncpy(noticeText, text, sizeof(noticeText) - 1);
    noticeText[sizeof(noticeText) - 1] = '\0';
    noticeAtMs = millis();
    Serial.printf("Frame: notice: %s\n", text);
}

void Frame::updateBanner() {
    BannerKind kind = BANNER_NONE;
    char text[200] = "";
    const char *button = "";
    bool buttonEnabled = true;

    switch (MachineService::link()) {
        case MachineService::LINK_NO_MACHINE:
            kind = BANNER_NO_MACHINE;
            if (MachineService::machineStatus() == SettingsService::MACHINE_NOT_WIFI) {
                snprintf(text, sizeof(text), "Machine %d is not a WiFi machine. Choose or set up one in Settings.",
                         SettingsService::machineSlot() + 1);
            } else {
                snprintf(text, sizeof(text), "No machine is set up. Add one (WiFi) in Settings.");
            }
            button = "Settings";
            break;
        case MachineService::LINK_WIFI:
            kind = BANNER_LINK;
            if (MachineService::everOnline()) {
                snprintf(text, sizeof(text), "WiFi lost. Reconnecting to %s...%s", MachineService::wifiName(),
                         MachineService::jobRunningAtDrop() ? "\nThe job continues on the controller." : "");
            } else {
                snprintf(text, sizeof(text), "Connecting to WiFi %s...", MachineService::wifiName());
            }
            button = "Reconnect";
            break;
        case MachineService::LINK_CONTROLLER: {
            kind = BANNER_LINK;
            const char *job = MachineService::jobRunningAtDrop() ? "\nThe job continues on the controller." : "";
            if (MachineService::waitingForReconnect()) {
                // The automatic retries are used up (D46)
                if (MachineService::everOnline()) {
                    snprintf(text, sizeof(text), "Connection lost. Press Reconnect to try again.%s", job);
                } else {
                    snprintf(text, sizeof(text), "No answer from %s. Press Reconnect to try again.",
                             MachineService::machineName());
                }
            } else if (MachineService::attemptInProgress()) {
                // Reconnect could do nothing now, so it is shown disabled
                buttonEnabled = false;
                if (MachineService::everOnline()) {
                    snprintf(text, sizeof(text), "Connection lost. Reconnecting...%s", job);
                } else {
                    snprintf(text, sizeof(text), "Connecting to %s...", MachineService::machineName());
                }
            } else {
                int seconds = (int)((MachineService::msToNextAttempt() + 999) / 1000);
                if (MachineService::everOnline()) {
                    snprintf(text, sizeof(text), "Connection lost. Next attempt in %d s.%s", seconds, job);
                } else {
                    snprintf(text, sizeof(text), "No answer from %s. Next attempt in %d s.",
                             MachineService::machineName(), seconds);
                }
            }
            button = "Reconnect";
            break;
        }
        case MachineService::LINK_ONLINE:
            if (MachineService::status().state == STATE_ALARM) {
                kind = BANNER_ALARM;
                const char *alarm = MachineService::status().alarm_message;
                // The client's text already starts with "Alarm <n>:"
                snprintf(text, sizeof(text), "%s", alarm[0] ? alarm : "ALARM");
                button = "Unlock";
            }
            break;
    }

    bool kindChanged = kind != bannerKind;
    if (kindChanged) {
        bannerKind = kind;
        if (kind == BANNER_NONE) {
            lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(banner, kind == BANNER_ALARM ? BANNER_ALARM_BG : BANNER_LINK_BG, 0);
        }
    }
    if (kind == BANNER_NONE) return;

    setText(bannerText, text);
    setText(bannerButtonLabel, button);
    if (kindChanged || buttonEnabled != bannerButtonEnabled) {
        bannerButtonEnabled = buttonEnabled;
        if (buttonEnabled) {
            lv_obj_clear_state(bannerButton, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(bannerButton, kind == BANNER_ALARM ? UITheme::BTN_UNLOCK : UITheme::BTN_CONNECT, 0);
            lv_obj_set_style_text_color(bannerButtonLabel, lv_color_white(), 0);
        } else {
            lv_obj_add_state(bannerButton, LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(bannerButton, UITheme::BG_BUTTON, 0);
            lv_obj_set_style_text_color(bannerButtonLabel, UITheme::TEXT_DISABLED, 0);
        }
    }
}

void Frame::onTabClicked(lv_event_t *e) {
    show((ScreenId)(intptr_t)lv_event_get_user_data(e));
}

void Frame::onStopClicked(lv_event_t *e) {
    if (JobService::active()) {
        JobService::abort();
    } else {
        MachineService::stop();
    }
}

void Frame::onBannerButtonClicked(lv_event_t *e) {
    switch (bannerKind) {
        case BANNER_ALARM:      MachineService::unlock(); break;
        case BANNER_LINK:       MachineService::reconnect(); break;
        case BANNER_NO_MACHINE: show(SCREEN_SETTINGS); break;
        default: break;
    }
}
