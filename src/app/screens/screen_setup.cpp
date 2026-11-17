#include "app/screens/screens.h"
#include "app/widgets.h"
#include "services/machine_service.h"
#include "services/settings_service.h"
#include "ui/ui_theme.h"
#include <Arduino.h>
#include <math.h>

// The Setup screen (specs/03-screens.md section 2), left to right in the
// order of a session. Content area 800 x 376:
//
//   y   0..64   Home all, Zero all | Home Z, Zero XY, Zero Z | Unlock, Reset
//   y  68..88   step labels; limit and probe pins
//   y  92..372  XY steps and jog pad with feed | Z steps, Z+/Z- | laser,
//               go to zero, probe Z, job strip
//
// Which controls are enabled follows MachineService::can*(), the same
// checks the actions make.

using namespace Widgets;

namespace {

const lv_font_t *const FONT_BIG = &lv_font_montserrat_20;
const lv_font_t *const FONT_MID = &lv_font_montserrat_18;
const lv_font_t *const FONT_SMALL = &lv_font_montserrat_14;

const int FEED_STEP = 100;               // mm/min per [-] or [+]
const int FEED_MIN = 100;
const uint32_t FEED_SAVE_DELAY_MS = 2000;  // after the last change

const int RIGHT_X = 504;                 // the right-hand column
const int RIGHT_W = 288;

SettingsService::Jog jog;

// Buttons, by what enables them
lv_obj_t *homeButtons[2];                // Home all, Home Z
lv_obj_t *zeroButtons[3];                // Zero all, Zero XY, Zero Z
lv_obj_t *unlockButton;
lv_obj_t *resetButton;
lv_obj_t *xyStepButtons[SettingsService::MAX_STEPS];
lv_obj_t *zStepButtons[SettingsService::MAX_STEPS];
lv_obj_t *padButtons[8];
lv_obj_t *zButtons[2];
lv_obj_t *feedButtons[2];
lv_obj_t *laserButtons[2];
lv_obj_t *goZeroButton;
lv_obj_t *probeOpenButton;

lv_obj_t *feedLabel;
int xyStepIndex = 0;
int zStepIndex = 0;
bool feedDirty = false;
uint32_t feedChangedMs = 0;

// Limit and probe pins
lv_obj_t *pinDots[4];                    // X, Y, Z limit, probe
bool pinShown[4];

// Probe Z panel
lv_obj_t *probePanel;
lv_obj_t *probeInfo;
lv_obj_t *probeResult;
lv_obj_t *probeButton;
uint32_t probeCountAtStart = 0;
bool probeStarted = false;               // a probe was started from the panel since it opened

// --- Helpers -----------------------------------------------------------------

int nearestIndex(const float *values, int count, float value) {
    int best = 0;
    for (int i = 1; i < count; i++) {
        if (fabsf(values[i] - value) < fabsf(values[best] - value)) best = i;
    }
    return best;
}

void formatStep(char *buf, size_t size, float step) {
    // 100, 10, 1, 0.1, 0.05: as few digits as the value needs
    snprintf(buf, size, "%g", step);
}

void showStepSelection(lv_obj_t *const *buttons, int count, int selected) {
    for (int i = 0; i < count; i++) {
        setButtonColor(buttons[i], i == selected ? UITheme::ACCENT_PRIMARY : UITheme::BG_BUTTON);
    }
}

void showFeed() {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", jog.xyFeed);
    setText(feedLabel, buf);
}

// A row or column of step buttons that touch each other, like one control
void makeSteps(lv_obj_t *parent, int x, int y, int w, int h, bool vertical, const float *steps, int count,
               lv_obj_t **out, lv_event_cb_t onClick) {
    char text[12];
    for (int i = 0; i < count; i++) {
        formatStep(text, sizeof(text), steps[i]);
        int bx = vertical ? x : x + i * w;
        int by = vertical ? y + i * h : y;
        lv_obj_t *b = button(parent, bx, by, w, h, text, FONT_MID, onClick, (void *)(intptr_t)i);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, UITheme::BG_DARKER, 0);
        out[i] = b;
    }
}

lv_obj_t *pinDot(lv_obj_t *parent, int x, int y) {
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, UITheme::LED_OFF, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    return dot;
}

// --- Events ------------------------------------------------------------------

void onHomeAll(lv_event_t *) { MachineService::homeAll(); }
void onHomeZ(lv_event_t *) { MachineService::homeZ(); }
void onZero(lv_event_t *e) { MachineService::zero((uint8_t)(intptr_t)lv_event_get_user_data(e)); }
void onUnlock(lv_event_t *) { MachineService::unlock(); }
void onReset(lv_event_t *) { MachineService::reset(); }

void onXYStep(lv_event_t *e) {
    xyStepIndex = (int)(intptr_t)lv_event_get_user_data(e);
    showStepSelection(xyStepButtons, jog.xyStepCount, xyStepIndex);
    SettingsService::saveJogXYStep(jog.xySteps[xyStepIndex]);
}

void onZStep(lv_event_t *e) {
    zStepIndex = (int)(intptr_t)lv_event_get_user_data(e);
    showStepSelection(zStepButtons, jog.zStepCount, zStepIndex);
    SettingsService::saveJogZStep(jog.zSteps[zStepIndex]);
}

// User data: (dx + 1) * 3 + (dy + 1), each of dx, dy being -1, 0 or 1
void onPad(lv_event_t *e) {
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    float step = jog.xySteps[xyStepIndex];
    MachineService::jogBy((code / 3 - 1) * step, (code % 3 - 1) * step, 0, jog.xyFeed);
}

void onZ(lv_event_t *e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    MachineService::jogBy(0, 0, dir * jog.zSteps[zStepIndex], jog.zFeed);
}

void onFeed(lv_event_t *e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    int feed = jog.xyFeed + dir * FEED_STEP;
    if (feed < FEED_MIN) feed = FEED_MIN;
    if (feed == jog.xyFeed) return;
    jog.xyFeed = feed;
    showFeed();
    // Saved once the presses stop, not on each one
    feedDirty = true;
    feedChangedMs = millis();
}

// User data: 1 laser -> bit, -1 bit -> laser
void onLaser(lv_event_t *e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    SettingsService::LaserOffset o = SettingsService::laserOffset();
    MachineService::jogBy(dir * o.x, dir * o.y, 0, jog.xyFeed);
}

void onGoZero(lv_event_t *) { MachineService::goToXYZero(jog.xyFeed); }

void onProbeOpen(lv_event_t *) {
    SettingsService::Probe p = SettingsService::probe();
    char buf[96];
    snprintf(buf, sizeof(buf), "Feed %d mm/min, up to %d mm down.\nSet in the classic UI, Settings > Probe.",
             p.feed, p.maxDistance);
    setText(probeInfo, buf);
    setText(probeResult, "");
    probeStarted = false;
    lv_obj_clear_flag(probePanel, LV_OBJ_FLAG_HIDDEN);
}

void onProbeClose(lv_event_t *) { lv_obj_add_flag(probePanel, LV_OBJ_FLAG_HIDDEN); }

void onProbe(lv_event_t *) {
    if (!MachineService::canZero()) return;
    SettingsService::Probe p = SettingsService::probe();
    probeCountAtStart = MachineService::lastProbe().count;
    probeStarted = true;
    MachineService::probeZ(p.maxDistance, p.feed);
}

// --- Building ------------------------------------------------------------------

void createTopRow(lv_obj_t *parent) {
    // The everyday pair, larger and in the accent colour
    homeButtons[0] = button(parent, 8, 8, 120, 56, "Home all", FONT_BIG, onHomeAll);
    zeroButtons[0] = button(parent, 136, 8, 120, 56, "Zero all", FONT_BIG, onZero,
                            (void *)(intptr_t)MachineService::AXES_XYZ);
    setButtonColor(homeButtons[0], UITheme::ACCENT_PRIMARY);
    setButtonColor(zeroButtons[0], UITheme::ACCENT_PRIMARY);

    homeButtons[1] = button(parent, 280, 8, 92, 56, "Home Z", FONT_MID, onHomeZ);
    zeroButtons[1] = button(parent, 380, 8, 92, 56, "Zero XY", FONT_MID, onZero,
                            (void *)(intptr_t)MachineService::AXES_XY);
    zeroButtons[2] = button(parent, 480, 8, 92, 56, "Zero Z", FONT_MID, onZero,
                            (void *)(intptr_t)MachineService::AXIS_Z);

    // Far right, away from the jog pad (S2)
    unlockButton = button(parent, 596, 8, 92, 56, "Unlock", FONT_MID, onUnlock);
    resetButton = button(parent, 696, 8, 96, 56, "Reset", FONT_MID, onReset);
    lv_obj_set_style_text_color(resetButton, UITheme::STATE_ALARM, 0);
}

void createPins(lv_obj_t *parent) {
    label(parent, RIGHT_X, 70, FONT_SMALL, UITheme::TEXT_MEDIUM, "Limits");
    static const char *const names[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; i++) {
        pinDots[i] = pinDot(parent, RIGHT_X + 56 + i * 40, 73);
        label(parent, RIGHT_X + 72 + i * 40, 70, FONT_SMALL, UITheme::TEXT_MEDIUM, names[i]);
    }
    label(parent, RIGHT_X + 186, 70, FONT_SMALL, UITheme::TEXT_MEDIUM, "Probe");
    pinDots[3] = pinDot(parent, RIGHT_X + 236, 73);
}

void createXY(lv_obj_t *parent) {
    label(parent, 8, 70, FONT_SMALL, UITheme::TEXT_MEDIUM, "XY step (mm)");
    makeSteps(parent, 8, 92, 64, 56, false, jog.xySteps, jog.xyStepCount, xyStepButtons, onXYStep);

    // 3 x 3 pad without a centre button; Y+ is up
    static const char *const texts[3][3] = {
        {"X-\nY+", "Y+", "X+\nY+"},
        {"X-", nullptr, "X+"},
        {"X-\nY-", "Y-", "X+\nY-"},
    };
    int n = 0;
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            const char *text = texts[row][col];
            if (!text) continue;
            int dx = col - 1;
            int dy = 1 - row;
            bool diagonal = dx != 0 && dy != 0;
            padButtons[n++] = button(parent, 8 + col * 82, 156 + row * 70, 76, 64, text,
                                     diagonal ? FONT_SMALL : FONT_BIG, onPad,
                                     (void *)(intptr_t)((dx + 1) * 3 + (dy + 1)));
        }
    }

    // XY feed beside the pad: [+] above, value, [-] below
    feedButtons[1] = button(parent, 256, 156, 72, 64, "+", FONT_BIG, onFeed, (void *)(intptr_t)1);
    feedLabel = lv_label_create(parent);
    lv_obj_set_style_text_font(feedLabel, FONT_BIG, 0);
    lv_obj_set_style_text_color(feedLabel, UITheme::TEXT_LIGHT, 0);
    lv_obj_set_style_text_align(feedLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(feedLabel, 72);
    lv_obj_set_pos(feedLabel, 256, 234);
    lv_obj_t *unit = label(parent, 256, 260, FONT_SMALL, UITheme::TEXT_MEDIUM, "mm/min");
    lv_obj_set_width(unit, 72);
    lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);
    feedButtons[0] = button(parent, 256, 296, 72, 64, "-", FONT_BIG, onFeed, (void *)(intptr_t)-1);
    showFeed();

    showStepSelection(xyStepButtons, jog.xyStepCount, xyStepIndex);
}

void createZ(lv_obj_t *parent) {
    label(parent, 344, 70, FONT_SMALL, UITheme::TEXT_MEDIUM, "Z step");
    makeSteps(parent, 344, 92, 64, 56, true, jog.zSteps, jog.zStepCount, zStepButtons, onZStep);
    zButtons[0] = button(parent, 416, 92, 76, 136, "Z+", FONT_BIG, onZ, (void *)(intptr_t)1);
    zButtons[1] = button(parent, 416, 236, 76, 136, "Z-", FONT_BIG, onZ, (void *)(intptr_t)-1);
    showStepSelection(zStepButtons, jog.zStepCount, zStepIndex);
}

void createRight(lv_obj_t *parent) {
    laserButtons[0] = button(parent, RIGHT_X, 92, 140, 56, "Laser " LV_SYMBOL_RIGHT " bit", FONT_MID, onLaser,
                             (void *)(intptr_t)1);
    laserButtons[1] = button(parent, RIGHT_X + 148, 92, 140, 56, "Bit " LV_SYMBOL_RIGHT " laser", FONT_MID,
                             onLaser, (void *)(intptr_t)-1);
    goZeroButton = button(parent, RIGHT_X, 156, 140, 56, "Go to XY zero", FONT_MID, onGoZero);
    probeOpenButton = button(parent, RIGHT_X + 148, 156, 140, 56, "Probe Z ...", FONT_MID, onProbeOpen);

    // Job strip: the space is kept; jobs are chosen from M3 on (D33)
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_pos(line, RIGHT_X, 226);
    lv_obj_set_size(line, RIGHT_W, 1);
    lv_obj_set_style_bg_color(line, UITheme::BORDER_MEDIUM, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    label(parent, RIGHT_X, 240, FONT_MID, UITheme::TEXT_MEDIUM, "No job selected");
    label(parent, RIGHT_X, 268, FONT_SMALL, UITheme::TEXT_DISABLED, "Jobs are chosen here from M3 on.");
    lv_obj_t *choose = button(parent, RIGHT_X, 312, 140, 56, "Choose job", FONT_MID, nullptr);
    lv_obj_t *start = button(parent, RIGHT_X + 148, 312, 140, 56, "Start " LV_SYMBOL_RIGHT, FONT_MID, nullptr);
    setEnabled(choose, false);
    setEnabled(start, false);
}

void createProbePanel(lv_obj_t *parent) {
    // Covers the right-hand column while open
    probePanel = lv_obj_create(parent);
    lv_obj_set_pos(probePanel, RIGHT_X - 4, 88);
    lv_obj_set_size(probePanel, RIGHT_W + 8, 288);
    lv_obj_set_style_bg_color(probePanel, UITheme::BG_DARK, 0);
    lv_obj_set_style_border_color(probePanel, UITheme::BORDER_MEDIUM, 0);
    lv_obj_set_style_border_width(probePanel, 1, 0);
    lv_obj_set_style_radius(probePanel, 6, 0);
    lv_obj_set_style_pad_all(probePanel, 0, 0);
    lv_obj_clear_flag(probePanel, LV_OBJ_FLAG_SCROLLABLE);

    label(probePanel, 12, 10, FONT_BIG, UITheme::TEXT_LIGHT, "Probe Z");
    probeInfo = label(probePanel, 12, 42, FONT_SMALL, UITheme::TEXT_MEDIUM, "");
    lv_obj_set_width(probeInfo, RIGHT_W - 16);
    lv_label_set_long_mode(probeInfo, LV_LABEL_LONG_WRAP);
    probeResult = label(probePanel, 12, 96, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(probeResult, RIGHT_W - 16);
    lv_label_set_long_mode(probeResult, LV_LABEL_LONG_WRAP);

    probeButton = button(probePanel, 8, 220, 136, 56, "Probe", FONT_BIG, onProbe);
    setButtonColor(probeButton, UITheme::BTN_PROBE);
    button(probePanel, 152, 220, 136, 56, "Close", FONT_BIG, onProbeClose);

    lv_obj_add_flag(probePanel, LV_OBJ_FLAG_HIDDEN);
}

void updateProbePanel() {
    if (lv_obj_has_flag(probePanel, LV_OBJ_FLAG_HIDDEN)) return;
    setEnabled(probeButton, MachineService::canZero());
    if (!probeStarted) return;

    const MachineService::ProbeResult &r = MachineService::lastProbe();
    char buf[96];
    if (r.count == probeCountAtStart) {
        snprintf(buf, sizeof(buf), MachineService::probeActive() ? "Probing..." : "Probe stopped.");
    } else if (r.success) {
        snprintf(buf, sizeof(buf), "Touched at Z %.3f work\n(%.3f machine)", r.workZ, r.z);
    } else {
        snprintf(buf, sizeof(buf), "No contact within %d mm.%s", SettingsService::probe().maxDistance,
                 MachineService::online() && MachineService::status().state == STATE_ALARM ? "\nUnlock to go on."
                                                                                           : "");
    }
    setText(probeResult, buf);
}

}  // namespace

void SetupScreen::create(lv_obj_t *parent) {
    jog = SettingsService::jog();
    xyStepIndex = nearestIndex(jog.xySteps, jog.xyStepCount, jog.xyStep);
    zStepIndex = nearestIndex(jog.zSteps, jog.zStepCount, jog.zStep);

    createTopRow(parent);
    createPins(parent);
    createXY(parent);
    createZ(parent);
    createRight(parent);
    createProbePanel(parent);
}

void SetupScreen::update() {
    bool home = MachineService::canHome();
    bool zero = MachineService::canZero();
    bool move = MachineService::canMove();

    for (lv_obj_t *b : homeButtons) setEnabled(b, home);
    for (lv_obj_t *b : zeroButtons) setEnabled(b, zero);
    setEnabled(probeOpenButton, zero);
    for (int i = 0; i < jog.xyStepCount; i++) setEnabled(xyStepButtons[i], move);
    for (int i = 0; i < jog.zStepCount; i++) setEnabled(zStepButtons[i], move);
    for (lv_obj_t *b : padButtons) setEnabled(b, move);
    for (lv_obj_t *b : zButtons) setEnabled(b, move);
    for (lv_obj_t *b : feedButtons) setEnabled(b, move);
    for (lv_obj_t *b : laserButtons) setEnabled(b, move);
    setEnabled(goZeroButton, move);
    setEnabled(unlockButton, MachineService::canUnlock());
    setEnabled(resetButton, MachineService::canReset());

    // Unlock stands out in ALARM
    bool alarm = MachineService::online() && MachineService::status().state == STATE_ALARM;
    setButtonColor(unlockButton, alarm ? UITheme::BTN_UNLOCK : UITheme::BG_BUTTON);

    // Pins: lit while active
    const FluidNCStatus &s = MachineService::status();
    bool online = MachineService::online();
    const bool pins[4] = {online && s.pin_limit_x, online && s.pin_limit_y, online && s.pin_limit_z,
                          online && s.pin_probe};
    for (int i = 0; i < 4; i++) {
        if (pins[i] == pinShown[i]) continue;
        pinShown[i] = pins[i];
        lv_obj_set_style_bg_color(pinDots[i], pins[i] ? UITheme::STATE_ALARM : UITheme::LED_OFF, 0);
    }

    updateProbePanel();

    if (feedDirty && millis() - feedChangedMs >= FEED_SAVE_DELAY_MS) {
        feedDirty = false;
        SettingsService::saveJogXYFeed(jog.xyFeed);
    }
}
