#include "app/screens/screens.h"
#include "app/frame.h"
#include "app/widgets.h"
#include "app/icons/icons.h"
#include "services/machine_service.h"
#include "services/settings_service.h"
#include "ui/ui_theme.h"
#include <Arduino.h>
#include <math.h>

// The Setup screen (specs/03-screens.md section 2), left to right in the
// order of a session. Content area 800 x 360:
//
//   y   8..64   Home all, Zero all | Home Z, Zero Z, Home XY, Zero XY | Unlock, Reset
//   y  72..352  XY steps, jog pad | Z+/Z-, Z steps | pins, laser, probe
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

const int LOWER_Y = 72;                 // below the top row; 280 px high
const int STEP_W = 68, STEP_H = 56;     // step selector buttons
const int XY_STEP_X = 8, XY_STEP_H = 44; // XY steps, a column left of the pad,
const int MODE_Y = LOWER_Y + 232, MODE_H = 48;  // then the arrows / joystick switch
const int PAD_SIZE = 280;               // the jog pad or the joystick
const int PAD_X = 92, PAD_CELL = 88, PAD_GAP = 8;  // 3 x 3 square jog pad
const int DIVIDER1_X = 396;
const int Z_X = 436, Z_W = 72;          // Z+/Z-, then the Z steps
const int Z_STEP_X = Z_X + Z_W + 12;
const int DIVIDER2_X = 628;
const int RIGHT_X = 652, RIGHT_W = 140; // pins, laser, probe

SettingsService::Jog jog;

// Buttons, by what enables them
lv_obj_t *homeButtons[3];               // all, Z, XY
lv_obj_t *otherButtons[32];             // greyed in alarm and during a job
int otherCount = 0;
lv_obj_t *unlockButton;
lv_obj_t *resetButton;
lv_obj_t *xyStepButtons[SettingsService::MAX_STEPS];
lv_obj_t *zStepButtons[SettingsService::MAX_STEPS];
int xyStepIndex = 0;
int zStepIndex = 0;

// XY: the 3 x 3 pad or the joystick, switched by modeButton (D42)
lv_obj_t *padGroup;
lv_obj_t *stickGroup;
lv_obj_t *stickBase;                    // the circle; takes the touches
lv_obj_t *stickKnob;
lv_obj_t *modeButton;
lv_obj_t *modeIcon;
bool stickMode = false;

// Joystick jogging, as in the classic joystick tab: a short jog every
// JOG_INTERVAL_MS while held, cancelled when let go
const uint32_t JOG_INTERVAL_MS = 50;
bool stickRefused = false;              // this press was refused; ignore its drag
bool stickJogging = false;
uint32_t lastStickJogMs = 0;

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
        out[i] = b;
    }
}

// The jog feed, centred in a box: the value, "mm/min" under it
void feedLabel(lv_obj_t *parent, int x, int y, int w, int h, int feed) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", feed);
    lv_obj_t *value = label(parent, x, y + h / 2 - 22, FONT_MID, UITheme::TEXT_LIGHT, buf);
    lv_obj_t *unit = label(parent, x, y + h / 2 + 2, FONT_SMALL, UITheme::TEXT_MEDIUM, "mm/min");
    for (lv_obj_t *l : {value, unit}) {
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
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

// The chosen step lasts until a restart; the one Setup starts with is set
// in Settings (D45)
void onXYStep(lv_event_t *e) {
    xyStepIndex = (int)(intptr_t)lv_event_get_user_data(e);
    showSteps();
}

void onZStep(lv_event_t *e) {
    zStepIndex = (int)(intptr_t)lv_event_get_user_data(e);
    showSteps();
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

void showXYMode() {
    if (stickMode) {
        lv_obj_add_flag(padGroup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(stickGroup, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(stickGroup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(padGroup, LV_OBJ_FLAG_HIDDEN);
    }
    // The button shows the mode it switches to
    lv_image_set_src(modeIcon, stickMode ? &ICON_ARROWS : &ICON_JOYSTICK);
}

void onXYMode(lv_event_t *) {
    stickMode = !stickMode;
    showXYMode();
}

// Quadratic response, as in the classic UI: fine control near the centre
float stickCurve(float percent) {
    if (percent < 0.1f) return 0.0f;
    float n = percent / 100.0f;
    return n * n * 100.0f;
}

void stopStick() {
    lv_obj_center(stickKnob);
    if (stickJogging) MachineService::cancelJog();
    stickJogging = false;
}

// Touches anywhere on the circle move the knob there
void onStick(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        stickRefused = !allowed(MachineService::canMove());
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        stopStick();
        return;
    }
    if (code != LV_EVENT_PRESSING || stickRefused) return;

    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(stickBase, &a);
    float dx = p.x - (a.x1 + a.x2) / 2.0f;
    float dy = p.y - (a.y1 + a.y2) / 2.0f;
    float dist = sqrtf(dx * dx + dy * dy);
    float maxR = lv_obj_get_width(stickBase) / 2 - lv_obj_get_width(stickKnob) / 2 - 5;
    if (dist > maxR) {
        dx *= maxR / dist;
        dy *= maxR / dist;
    }
    lv_obj_align(stickKnob, LV_ALIGN_CENTER, (int32_t)dx, (int32_t)dy);

    uint32_t now = millis();
    if (stickJogging && now - lastStickJogMs < JOG_INTERVAL_MS) return;
    float percent = dist >= maxR ? 100.0f : dist * 100.0f / maxR;
    float speed = stickCurve(percent) / 100.0f * jog.xyMaxFeed;  // mm/min
    float angle = atan2f(dy, dx);
    float vx = speed * cosf(angle), vy = -speed * sinf(angle);    // Y+ is up
    // Each jog covers the time since the last one, at most a few intervals
    float dt = stickJogging ? fminf((now - lastStickJogMs) / 1000.0f, 0.2f) : JOG_INTERVAL_MS / 1000.0f;
    float mx = vx / 60.0f * dt, my = vy / 60.0f * dt;
    if (speed < 1.0f || (fabsf(mx) < 0.0005f && fabsf(my) < 0.0005f)) return;  // below the 0.001 mm sent
    MachineService::jogBy(fabsf(mx) < 0.0005f ? 0 : mx, fabsf(my) < 0.0005f ? 0 : my, 0, (int)ceilf(speed));
    stickJogging = true;
    lastStickJogMs = now;
}

void onProbeOpen(lv_event_t *) {
    SettingsService::Probe p = SettingsService::probe();
    char buf[200];
    snprintf(buf, sizeof(buf),
             "Moves Z down at %d mm/min until the probe touches, at most %d mm, "
             "then raises Z %g mm. Feed and distance are in the classic UI, Settings > Probe.",
             p.feed, p.maxDistance, p.backOff);
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
    MachineService::probeZ(p.maxDistance, p.feed, p.backOff);
}

// --- Building ------------------------------------------------------------------

void createTopRow(lv_obj_t *parent) {
    // The everyday pair, wider
    homeButtons[0] = button(parent, 8, 8, 112, 56, LV_SYMBOL_HOME "  All", FONT_BIG, onHome, (void *)0);
    lv_obj_t *zeroAll = other(button(parent, 128, 8, 112, 56, "All", FONT_BIG, onZero,
                                     (void *)(intptr_t)MachineService::AXES_XYZ));

    // The occasional ones: Z, then XY
    homeButtons[1] = button(parent, 256, 8, 88, 56, LV_SYMBOL_HOME "  Z", FONT_MID, onHome, (void *)1);
    lv_obj_t *zeroZ = other(button(parent, 350, 8, 88, 56, "Z", FONT_MID, onZero,
                                   (void *)(intptr_t)MachineService::AXIS_Z));
    homeButtons[2] = button(parent, 444, 8, 88, 56, LV_SYMBOL_HOME "  XY", FONT_MID, onHome, (void *)2);
    lv_obj_t *zeroXY = other(button(parent, 538, 8, 88, 56, "XY", FONT_MID, onZero,
                                    (void *)(intptr_t)MachineService::AXES_XY));

    for (lv_obj_t *b : homeButtons) setButtonColor(b, COLOR_HOME);
    for (lv_obj_t *b : {zeroAll, zeroZ, zeroXY}) {
        setButtonColor(b, COLOR_ZERO);
        addIcon(b, &ICON_ZERO);
    }

    // Far right, away from the jog pad (S2)
    unlockButton = button(parent, 644, 8, 72, 56, "Unlock", FONT_MID, onUnlock);
    resetButton = button(parent, 720, 8, 72, 56, "Reset", FONT_MID, onReset);
    lv_obj_set_style_text_color(resetButton, LV_COLOR_MAKE(0xFF, 0x6B, 0x6B), 0);
}

void createXY(lv_obj_t *parent) {
    makeSteps(parent, XY_STEP_X, LOWER_Y, STEP_W, XY_STEP_H, true, jog.xySteps, jog.xyStepCount, xyStepButtons,
              onXYStep);

    modeButton = button(parent, XY_STEP_X, MODE_Y, STEP_W, MODE_H, "", FONT_MID, onXYMode);
    lv_obj_add_flag(lv_obj_get_child(modeButton, 0), LV_OBJ_FLAG_HIDDEN);
    modeIcon = addIcon(modeButton, &ICON_JOYSTICK);

    // Two groups in the same place; one is shown
    for (lv_obj_t **group : {&padGroup, &stickGroup}) {
        *group = lv_obj_create(parent);
        lv_obj_set_pos(*group, PAD_X, LOWER_Y);
        lv_obj_set_size(*group, PAD_SIZE, PAD_SIZE);
        lv_obj_set_style_bg_opa(*group, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(*group, 0, 0);
        lv_obj_set_style_pad_all(*group, 0, 0);
        lv_obj_clear_flag(*group, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(*group, LV_OBJ_FLAG_CLICKABLE);
    }

    // Joystick, as in the classic joystick tab: XY only, no speed display
    stickBase = lv_obj_create(stickGroup);
    lv_obj_set_size(stickBase, PAD_SIZE - 8, PAD_SIZE - 8);
    lv_obj_center(stickBase);
    lv_obj_set_style_radius(stickBase, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(stickBase, UITheme::JOYSTICK_BG, 0);
    lv_obj_set_style_border_width(stickBase, 2, 0);
    lv_obj_set_style_border_color(stickBase, UITheme::JOYSTICK_BORDER, 0);
    lv_obj_set_style_pad_all(stickBase, 0, 0);
    lv_obj_clear_flag(stickBase, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(stickBase, LV_OBJ_FLAG_SCROLL_CHAIN);
    for (int vertical = 0; vertical < 2; vertical++) {
        lv_obj_t *l = lv_obj_create(stickBase);
        lv_obj_set_size(l, vertical ? 2 : PAD_SIZE - 28, vertical ? PAD_SIZE - 28 : 2);
        lv_obj_set_style_bg_color(l, UITheme::JOYSTICK_LINE, 0);
        lv_obj_set_style_border_width(l, 0, 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(l);
    }
    stickKnob = lv_obj_create(stickBase);
    lv_obj_set_size(stickKnob, 90, 90);
    lv_obj_set_style_radius(stickKnob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(stickKnob, UITheme::AXIS_XY, 0);
    lv_obj_set_style_border_width(stickKnob, 3, 0);
    lv_obj_set_style_border_color(stickKnob, lv_color_white(), 0);
    lv_obj_set_style_bg_color(stickKnob, UITheme::BG_BUTTON, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(stickKnob, UITheme::TEXT_DISABLED, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(stickKnob, UITheme::TEXT_DISABLED, LV_STATE_DISABLED);
    lv_obj_clear_flag(stickKnob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(stickKnob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(stickKnob);
    lv_obj_t *knobLabel = lv_label_create(stickKnob);
    lv_label_set_text(knobLabel, "XY");
    lv_obj_set_style_text_font(knobLabel, FONT_BIG, 0);
    lv_obj_center(knobLabel);
    for (lv_event_code_t c : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(stickBase, onStick, c, nullptr);
    }

    // 3 x 3 square pad, Y+ up; the arrows as in the classic jog tab, turned
    // 45 degrees on the diagonals. The centre shows the XY jog feed.
    static const char *const icons[3][3] = {
        {LV_SYMBOL_UP, LV_SYMBOL_UP, LV_SYMBOL_UP},
        {LV_SYMBOL_LEFT, nullptr, LV_SYMBOL_RIGHT},
        {LV_SYMBOL_DOWN, LV_SYMBOL_DOWN, LV_SYMBOL_DOWN},
    };
    static const int16_t angles[3][3] = {{-450, 0, 450}, {0, 0, 0}, {450, 0, -450}};
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int x = col * (PAD_CELL + PAD_GAP);
            int y = row * (PAD_CELL + PAD_GAP);
            if (!icons[row][col]) {
                feedLabel(padGroup, x, y, PAD_CELL, PAD_CELL, jog.xyFeed);
                continue;
            }
            int dx = col - 1;
            int dy = 1 - row;
            lv_color_t color = dx && dy ? UITheme::AXIS_XY : (dx ? UITheme::AXIS_X : UITheme::AXIS_Y);
            other(iconButton(padGroup, x, y, PAD_CELL, PAD_CELL, icons[row][col], angles[row][col], nullptr, color,
                             onPad, (void *)(intptr_t)((dx + 1) * 3 + (dy + 1))));
        }
    }
}

void createZ(lv_obj_t *parent) {
    // Z+ and Z- with the Z jog feed between them
    const int h = 108, gap = 280 - 2 * h;
    other(iconButton(parent, Z_X, LOWER_Y, Z_W, h, LV_SYMBOL_UP, 0, "Z+", UITheme::AXIS_Z, onZ, (void *)1));
    feedLabel(parent, Z_X, LOWER_Y + h, Z_W, gap, jog.zFeed);
    other(iconButton(parent, Z_X, LOWER_Y + h + gap, Z_W, h, LV_SYMBOL_DOWN, 0, "Z-", UITheme::AXIS_Z, onZ,
                     (void *)(intptr_t)-1));
    makeSteps(parent, Z_STEP_X, LOWER_Y, STEP_W, STEP_H, true, jog.zSteps, jog.zStepCount, zStepButtons, onZStep);
    for (int i = 0; i < jog.zStepCount; i++) other(zStepButtons[i]);
}

void createRight(lv_obj_t *parent) {
    // Limit and probe pins
    label(parent, RIGHT_X, LOWER_Y, FONT_SMALL, UITheme::TEXT_MEDIUM, "Limits");
    static const char *const names[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; i++) {
        pinDots[i] = pinDot(parent, RIGHT_X + i * 46, LOWER_Y + 27);
        label(parent, RIGHT_X + 16 + i * 46, LOWER_Y + 24, FONT_SMALL, UITheme::TEXT_LIGHT, names[i]);
    }
    // The probe pin, dot first like the limits
    pinDots[3] = pinDot(parent, RIGHT_X, LOWER_Y + 51);
    label(parent, RIGHT_X + 16, LOWER_Y + 48, FONT_SMALL, UITheme::TEXT_LIGHT, "Probe");

    // Laser crosshair -> bit, and back (D21)
    lv_obj_t *toBit = other(button(parent, RIGHT_X, LOWER_Y + 80, RIGHT_W, 56, LV_SYMBOL_RIGHT, FONT_MID, onLaser,
                                   (void *)1));
    addIcon(toBit, &ICON_CROSSHAIR, true);
    addIcon(toBit, &ICON_BIT, false);
    lv_obj_t *toLaser = other(button(parent, RIGHT_X, LOWER_Y + 144, RIGHT_W, 56, LV_SYMBOL_RIGHT, FONT_MID,
                                     onLaser, (void *)(intptr_t)-1));
    addIcon(toLaser, &ICON_BIT, true);
    addIcon(toLaser, &ICON_CROSSHAIR, false);
    setButtonColor(toBit, COLOR_LASER);
    setButtonColor(toLaser, COLOR_LASER);

    other(button(parent, RIGHT_X, LOWER_Y + 224, RIGHT_W, 56, "Probe Z", FONT_MID, onProbeOpen));
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
    divider(parent, DIVIDER1_X, LOWER_Y + 4, 1, 272);
    createZ(parent);
    divider(parent, DIVIDER2_X, LOWER_Y + 4, 1, 272);
    createRight(parent);
    createProbePopup(parent);
    showSteps();
    showXYMode();
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
    // The XY steps don't apply to the joystick
    for (int i = 0; i < jog.xyStepCount; i++) setEnabled(xyStepButtons[i], usable && !stickMode);
    setEnabled(stickBase, usable);
    setEnabled(stickKnob, usable);
    if (!usable && stickJogging) stopStick();
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
