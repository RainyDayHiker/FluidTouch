#include "app/screens/screens.h"
#include "app/frame.h"
#include "app/widgets.h"
#include "services/machine_service.h"
#include "services/settings_service.h"
#include "ui/ui_theme.h"
#include <Arduino.h>
#include <math.h>

// The Setup screen (specs/03-screens.md section 2), left to right in the
// order of a session. Content area 800 x 360:
//
//   y   8..64   Home all, Zero all | Home Z, Zero Z, Home XY, Zero XY | Unlock, Reset
//   y  72..352  XY steps over the jog pad | Z+/Z-, Z steps | pins, laser, probe
//
// Buttons are greyed out only in states that last (offline, alarm, a job
// running), so nothing flickers while jogging. A press that can't be sent
// at the moment puts the reason in the status bar instead.

using namespace Widgets;

namespace {

const lv_font_t *const FONT_ICON = &lv_font_montserrat_32;
const lv_font_t *const FONT_BIG = &lv_font_montserrat_20;
const lv_font_t *const FONT_MID = &lv_font_montserrat_18;
const lv_font_t *const FONT_SMALL = &lv_font_montserrat_14;

// Home and zero each have a colour, so the two rows of pairs read at a glance
const lv_color_t COLOR_HOME = LV_COLOR_MAKE(0x2E, 0x7D, 0x32);
const lv_color_t COLOR_ZERO = LV_COLOR_MAKE(0xB2, 0x6A, 0x00);
const lv_color_t COLOR_LASER = LV_COLOR_MAKE(0x8E, 0x24, 0x4A);

const int PAD_X = 8, PAD_W = 400;       // XY steps and jog pad
const int Z_X = 428;                    // Z+/Z- then Z steps
const int RIGHT_X = 644, RIGHT_W = 148; // pins, laser, probe
const int LOWER_Y = 72;

SettingsService::Jog jog;

// Buttons, by what enables them
lv_obj_t *homeButtons[3];               // all, Z, XY
lv_obj_t *otherButtons[32];             // greyed in alarm and during a job
int otherCount = 0;
lv_obj_t *unlockButton;
lv_obj_t *resetButton;
lv_obj_t *xyStepButtons[SettingsService::MAX_STEPS];
lv_obj_t *zStepButtons[SettingsService::MAX_STEPS];
lv_obj_t *stepDisplay;                  // centre of the jog pad
int xyStepIndex = 0;
int zStepIndex = 0;

// Limit and probe pins
lv_obj_t *pinDots[4];                   // X, Y, Z limit, probe
bool pinShown[4];

// Probe Z pop-up
lv_obj_t *probeLayer;
lv_obj_t *probeInfo;
lv_obj_t *probeResult;
lv_obj_t *probeButton;
uint32_t probeCountAtStart = 0;
bool probeStarted = false;              // a probe was started since the pop-up opened

// --- Helpers -----------------------------------------------------------------

lv_obj_t *other(lv_obj_t *btn) {
    if (otherCount < (int)(sizeof(otherButtons) / sizeof(otherButtons[0]))) otherButtons[otherCount++] = btn;
    return btn;
}

int nearestIndex(const float *values, int count, float value) {
    int best = 0;
    for (int i = 1; i < count; i++) {
        if (fabsf(values[i] - value) < fabsf(values[best] - value)) best = i;
    }
    return best;
}

void showSteps() {
    for (int i = 0; i < jog.xyStepCount; i++) {
        setButtonColor(xyStepButtons[i], i == xyStepIndex ? UITheme::ACCENT_PRIMARY : UITheme::BG_BUTTON);
    }
    for (int i = 0; i < jog.zStepCount; i++) {
        setButtonColor(zStepButtons[i], i == zStepIndex ? UITheme::ACCENT_PRIMARY : UITheme::BG_BUTTON);
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%g mm", jog.xySteps[xyStepIndex]);
    setText(stepDisplay, buf);
}

// Steps that touch each other, like one control
void makeSteps(lv_obj_t *parent, int x, int y, int w, int h, bool vertical, const float *steps, int count,
               lv_obj_t **out, lv_event_cb_t onClick) {
    char text[12];
    for (int i = 0; i < count; i++) {
        snprintf(text, sizeof(text), "%g", steps[i]);
        lv_obj_t *b = button(parent, vertical ? x : x + i * w, vertical ? y + i * h : y, w, h, text, FONT_MID,
                             onClick, (void *)(intptr_t)i);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, UITheme::BG_DARKER, 0);
        out[i] = other(b);
    }
}

// A button showing a large icon, optionally turned (in 0.1 degree), with a
// small caption under it when given
lv_obj_t *iconButton(lv_obj_t *parent, int x, int y, int w, int h, const char *icon, int angle, const char *caption,
                     lv_color_t bg, lv_event_cb_t onClick, void *userData) {
    lv_obj_t *b = button(parent, x, y, w, h, icon, FONT_ICON, onClick, userData);
    setButtonColor(b, bg);
    lv_obj_t *iconLabel = lv_obj_get_child(b, 0);
    if (angle) {
        // A square box around the glyph, so the turned glyph is not clipped
        lv_obj_set_size(iconLabel, 44, 44);
        lv_obj_set_style_pad_top(iconLabel, 4, 0);
        lv_obj_center(iconLabel);
        lv_obj_set_style_transform_rotation(iconLabel, angle, 0);
        lv_obj_set_style_transform_pivot_x(iconLabel, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(iconLabel, lv_pct(50), 0);
    }
    if (caption) {
        lv_obj_align(iconLabel, LV_ALIGN_CENTER, 0, -12);
        lv_obj_t *cap = lv_label_create(b);
        lv_label_set_text(cap, caption);
        lv_obj_set_style_text_font(cap, FONT_MID, 0);
        lv_obj_align(cap, LV_ALIGN_CENTER, 0, 22);
    }
    return b;
}

lv_obj_t *divider(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, w, h);
    lv_obj_set_style_bg_color(line, UITheme::BORDER_MEDIUM, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    return line;
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

// Each press first checks whether its action can be sent now
bool allowed(bool can) {
    if (!can) Frame::notice(MachineService::busyReason());
    return can;
}

// --- Events ------------------------------------------------------------------

void onHome(lv_event_t *e) {
    if (!allowed(MachineService::canHome())) return;
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
        case 0: MachineService::homeAll(); break;
        case 1: MachineService::homeZ(); break;
        default: MachineService::homeXY(); break;
    }
}

void onZero(lv_event_t *e) {
    if (allowed(MachineService::canZero())) MachineService::zero((uint8_t)(intptr_t)lv_event_get_user_data(e));
}

void onUnlock(lv_event_t *) { MachineService::unlock(); }
void onReset(lv_event_t *) { MachineService::reset(); }

void onXYStep(lv_event_t *e) {
    xyStepIndex = (int)(intptr_t)lv_event_get_user_data(e);
    showSteps();
    SettingsService::saveJogXYStep(jog.xySteps[xyStepIndex]);
}

void onZStep(lv_event_t *e) {
    zStepIndex = (int)(intptr_t)lv_event_get_user_data(e);
    showSteps();
    SettingsService::saveJogZStep(jog.zSteps[zStepIndex]);
}

// User data: (dx + 1) * 3 + (dy + 1), each of dx, dy being -1, 0 or 1.
// Presses during a jog queue on FluidNC.
void onPad(lv_event_t *e) {
    if (!allowed(MachineService::canMove())) return;
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    float step = jog.xySteps[xyStepIndex];
    MachineService::jogBy((code / 3 - 1) * step, (code % 3 - 1) * step, 0, jog.xyFeed);
}

void onZ(lv_event_t *e) {
    if (!allowed(MachineService::canMove())) return;
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    MachineService::jogBy(0, 0, dir * jog.zSteps[zStepIndex], jog.zFeed);
}

// User data: 1 laser -> bit, -1 bit -> laser
void onLaser(lv_event_t *e) {
    if (!allowed(MachineService::canMove())) return;
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    SettingsService::LaserOffset o = SettingsService::laserOffset();
    MachineService::jogBy(dir * o.x, dir * o.y, 0, jog.xyFeed);
}

void onProbeOpen(lv_event_t *) {
    SettingsService::Probe p = SettingsService::probe();
    char buf[200];
    snprintf(buf, sizeof(buf),
             "Moves Z down at %d mm/min until the probe touches, at most %d mm. "
             "Feed and distance are in the classic UI, Settings > Probe.",
             p.feed, p.maxDistance);
    setText(probeInfo, buf);
    setText(probeResult, "");
    probeStarted = false;
    lv_obj_clear_flag(probeLayer, LV_OBJ_FLAG_HIDDEN);
}

void onProbeClose(lv_event_t *) { lv_obj_add_flag(probeLayer, LV_OBJ_FLAG_HIDDEN); }

void onProbe(lv_event_t *) {
    if (!allowed(MachineService::canZero())) return;
    SettingsService::Probe p = SettingsService::probe();
    probeCountAtStart = MachineService::lastProbe().count;
    probeStarted = true;
    MachineService::probeZ(p.maxDistance, p.feed);
}

// --- Building ------------------------------------------------------------------

void createTopRow(lv_obj_t *parent) {
    // The everyday pair, wider
    homeButtons[0] = button(parent, 8, 8, 112, 56, LV_SYMBOL_HOME "  All", FONT_BIG, onHome, (void *)0);
    other(button(parent, 128, 8, 112, 56, LV_SYMBOL_GPS "  All", FONT_BIG, onZero,
                 (void *)(intptr_t)MachineService::AXES_XYZ));

    // The occasional ones: Z, then XY
    homeButtons[1] = button(parent, 256, 8, 88, 56, LV_SYMBOL_HOME "  Z", FONT_MID, onHome, (void *)1);
    other(button(parent, 350, 8, 88, 56, LV_SYMBOL_GPS "  Z", FONT_MID, onZero,
                 (void *)(intptr_t)MachineService::AXIS_Z));
    homeButtons[2] = button(parent, 444, 8, 88, 56, LV_SYMBOL_HOME "  XY", FONT_MID, onHome, (void *)2);
    other(button(parent, 538, 8, 88, 56, LV_SYMBOL_GPS "  XY", FONT_MID, onZero,
                 (void *)(intptr_t)MachineService::AXES_XY));

    for (lv_obj_t *b : homeButtons) setButtonColor(b, COLOR_HOME);
    for (int i = 0; i < 3; i++) setButtonColor(otherButtons[i], COLOR_ZERO);

    // Far right, away from the jog pad (S2)
    unlockButton = button(parent, 644, 8, 72, 56, "Unlock", FONT_MID, onUnlock);
    resetButton = button(parent, 720, 8, 72, 56, "Reset", FONT_MID, onReset);
    lv_obj_set_style_text_color(resetButton, LV_COLOR_MAKE(0xFF, 0x6B, 0x6B), 0);
}

void createXY(lv_obj_t *parent) {
    makeSteps(parent, PAD_X, LOWER_Y, PAD_W / 5, 56, false, jog.xySteps, jog.xyStepCount, xyStepButtons, onXYStep);

    // 3 x 3 pad, Y+ up; the arrows as in the classic jog tab, turned 45
    // degrees on the diagonals. The centre shows the step.
    static const char *const icons[3][3] = {
        {LV_SYMBOL_UP, LV_SYMBOL_UP, LV_SYMBOL_UP},
        {LV_SYMBOL_LEFT, nullptr, LV_SYMBOL_RIGHT},
        {LV_SYMBOL_DOWN, LV_SYMBOL_DOWN, LV_SYMBOL_DOWN},
    };
    static const int16_t angles[3][3] = {{-450, 0, 450}, {0, 0, 0}, {450, 0, -450}};
    const int w = 128, h = 68, gap = 8;
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int x = PAD_X + col * (w + gap);
            int y = LOWER_Y + 64 + row * (h + 6);
            if (!icons[row][col]) {
                stepDisplay = label(parent, x, y + 24, FONT_MID, UITheme::TEXT_MEDIUM, "");
                lv_obj_set_width(stepDisplay, w);
                lv_obj_set_style_text_align(stepDisplay, LV_TEXT_ALIGN_CENTER, 0);
                continue;
            }
            int dx = col - 1;
            int dy = 1 - row;
            lv_color_t color = dx && dy ? UITheme::AXIS_XY : (dx ? UITheme::AXIS_X : UITheme::AXIS_Y);
            other(iconButton(parent, x, y, w, h, icons[row][col], angles[row][col], nullptr, color, onPad,
                             (void *)(intptr_t)((dx + 1) * 3 + (dy + 1))));
        }
    }
}

void createZ(lv_obj_t *parent) {
    other(iconButton(parent, Z_X, LOWER_Y, 100, 134, LV_SYMBOL_UP, 0, "Z+", UITheme::AXIS_Z, onZ, (void *)1));
    other(iconButton(parent, Z_X, LOWER_Y + 146, 100, 134, LV_SYMBOL_DOWN, 0, "Z-", UITheme::AXIS_Z, onZ,
                     (void *)(intptr_t)-1));
    makeSteps(parent, Z_X + 108, LOWER_Y, 88, 56, true, jog.zSteps, jog.zStepCount, zStepButtons, onZStep);
}

void createRight(lv_obj_t *parent) {
    // Limit and probe pins
    label(parent, RIGHT_X, LOWER_Y, FONT_SMALL, UITheme::TEXT_MEDIUM, "Limits");
    static const char *const names[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; i++) {
        pinDots[i] = pinDot(parent, RIGHT_X + i * 50, LOWER_Y + 27);
        label(parent, RIGHT_X + 16 + i * 50, LOWER_Y + 24, FONT_SMALL, UITheme::TEXT_LIGHT, names[i]);
    }
    label(parent, RIGHT_X, LOWER_Y + 48, FONT_SMALL, UITheme::TEXT_MEDIUM, "Probe");
    pinDots[3] = pinDot(parent, RIGHT_X + 50, LOWER_Y + 51);

    // Laser -> bit is the everyday move; bit -> laser is rare (D21), so small
    lv_obj_t *toBit = other(button(parent, RIGHT_X, LOWER_Y + 80, RIGHT_W, 56, "Laser " LV_SYMBOL_RIGHT " bit",
                                   FONT_MID, onLaser, (void *)1));
    lv_obj_t *toLaser = other(button(parent, RIGHT_X, LOWER_Y + 144, 72, 56, "b>l", FONT_MID, onLaser,
                                     (void *)(intptr_t)-1));
    setButtonColor(toBit, COLOR_LASER);
    setButtonColor(toLaser, COLOR_LASER);

    other(button(parent, RIGHT_X, LOWER_Y + 224, RIGHT_W, 56, "Probe Z ...", FONT_MID, onProbeOpen));
}

void createProbePopup(lv_obj_t *parent) {
    // A dimmed layer over the whole screen content takes the touches while open
    probeLayer = lv_obj_create(parent);
    lv_obj_set_pos(probeLayer, 0, 0);
    lv_obj_set_size(probeLayer, 800, Frame::CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(probeLayer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(probeLayer, LV_OPA_60, 0);
    lv_obj_set_style_border_width(probeLayer, 0, 0);
    lv_obj_set_style_radius(probeLayer, 0, 0);
    lv_obj_clear_flag(probeLayer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *panel = lv_obj_create(probeLayer);
    lv_obj_set_size(panel, 440, 268);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, UITheme::BG_MEDIUM, 0);
    lv_obj_set_style_border_color(panel, UITheme::BORDER_LIGHT, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    label(panel, 16, 12, FONT_BIG, UITheme::TEXT_LIGHT, "Probe Z");
    probeInfo = label(panel, 16, 44, FONT_SMALL, UITheme::TEXT_MEDIUM, "");
    lv_obj_set_width(probeInfo, 408);
    lv_label_set_long_mode(probeInfo, LV_LABEL_LONG_WRAP);
    probeResult = label(panel, 16, 108, FONT_BIG, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(probeResult, 408);
    lv_label_set_long_mode(probeResult, LV_LABEL_LONG_WRAP);

    probeButton = button(panel, 16, 196, 196, 56, "Probe", FONT_BIG, onProbe);
    setButtonColor(probeButton, UITheme::BTN_PROBE);
    button(panel, 228, 196, 196, 56, "Close", FONT_BIG, onProbeClose);

    lv_obj_add_flag(probeLayer, LV_OBJ_FLAG_HIDDEN);
}

void updateProbePopup(bool usable) {
    if (lv_obj_has_flag(probeLayer, LV_OBJ_FLAG_HIDDEN)) return;
    setEnabled(probeButton, usable);
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
    divider(parent, 8, LOWER_Y - 4 - 1, 784, 1);
    createXY(parent);
    divider(parent, Z_X - 11, LOWER_Y + 4, 1, 272);
    createZ(parent);
    divider(parent, RIGHT_X - 11, LOWER_Y + 4, 1, 272);
    createRight(parent);
    createProbePopup(parent);
    showSteps();
}

void SetupScreen::update() {
    // Greyed out only in lasting states (specs/03-screens.md section 2)
    bool online = MachineService::online();
    const FluidNCStatus &s = MachineService::status();
    bool job = MachineService::jobActive();
    bool alarm = online && s.state == STATE_ALARM;
    bool usable = online && !job && !alarm;

    for (lv_obj_t *b : homeButtons) setEnabled(b, online && !job);
    for (int i = 0; i < otherCount; i++) setEnabled(otherButtons[i], usable);
    setEnabled(unlockButton, MachineService::canUnlock());
    setEnabled(resetButton, MachineService::canReset());
    setButtonColor(unlockButton, alarm ? UITheme::BTN_UNLOCK : UITheme::BG_BUTTON);

    const bool pins[4] = {online && s.pin_limit_x, online && s.pin_limit_y, online && s.pin_limit_z,
                          online && s.pin_probe};
    for (int i = 0; i < 4; i++) {
        if (pins[i] == pinShown[i]) continue;
        pinShown[i] = pins[i];
        lv_obj_set_style_bg_color(pinDots[i], pins[i] ? UITheme::STATE_ALARM : UITheme::LED_OFF, 0);
    }

    updateProbePopup(usable);
}
