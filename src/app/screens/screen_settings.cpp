#include "app/screens/screens.h"
#include "app/entry.h"
#include "app/frame.h"
#include "app/ui_mode.h"
#include "app/widgets.h"
#include "app/screens/draw_test.h"
#include "config.h"
#include "core/power_manager.h"
#include "services/position_log.h"
#include "services/machine_service.h"
#include "services/settings_service.h"
#include "ui/ui_theme.h"
#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include <string.h>

// The Settings screen (specs/03-screens.md section 6). Content area 800 x 360:
//
//   x   8..172  page buttons: Machine, Jog & Probe, Display, Tools
//   x 184..800  the chosen page, scrolling when taller than the screen
//
// Values are edited in drafts and saved when the screen is left (D55).

using namespace Widgets;

namespace {

const lv_font_t *const FONT_BIG = &lv_font_montserrat_20;
const lv_font_t *const FONT_MID = &lv_font_montserrat_18;
const lv_font_t *const FONT_CHOICE = &lv_font_montserrat_16;
const lv_font_t *const FONT_SMALL = &lv_font_montserrat_14;

const int NAV_X = 8, NAV_W = 164, NAV_H = 64, NAV_GAP = 8;
const int PAGE_X = 184, PAGE_W = 800 - PAGE_X;
const int ROW_H = 60;                    // one labelled value
const int VALUE_X = 300, VALUE_W = 200;  // the value's button in a row
const int CONTENT_W = PAGE_W - 16;       // room left for the scrollbar

enum Page { PAGE_MACHINE, PAGE_JOG, PAGE_DISPLAY, PAGE_TOOLS, PAGE_COUNT };
const char *const PAGE_NAMES[PAGE_COUNT] = {"Machine", "Jog & Probe", "Display", "Tools"};

lv_obj_t *screenParent;
lv_obj_t *navButtons[PAGE_COUNT];
lv_obj_t *pages[PAGE_COUNT];
int pageShown = PAGE_MACHINE;

// --- Drafts -------------------------------------------------------------------

const int STEPS = SettingsService::MAX_STEPS;

MachineConfig machineDrafts[MAX_MACHINES];
bool slotConfigured[MAX_MACHINES];
bool slotEdited[MAX_MACHINES];
int draftSlot;

// Steps: 0 = no step there; the list is closed up when saved
struct JogDraft {
    float xySteps[STEPS];
    float zSteps[STEPS];
    int xyStart, zStart;  // index of the step Setup starts with
    float xyFeed, zFeed, xyMaxFeed;
} jogDraft;
float probeFeed, probeDistance, probeBack;
float laserX, laserY;
SettingsService::Display displayDraft;

// --- Fields: a value shown on a button, changed in a pop-up ---------------------

struct Field {
    enum Kind : uint8_t { NUMBER, TEXT, PASSWORD };
    const char *title;
    const char *unit;
    Kind kind;
    float *number;   // NUMBER
    char *text;      // TEXT, PASSWORD
    size_t maxLen;
    float min, max;
    int decimals;
    lv_obj_t *button;
    void (*changed)(Field &);
};

Field fields[40];
int fieldCount = 0;

// Machine fields point into the draft of the slot shown
float portDraft;
Field *nameField, *ssidField, *passwordField, *urlField, *portField;

void showField(Field &f) {
    char buf[80];
    if (f.kind == Field::NUMBER) {
        Entry::formatNumber(buf, sizeof(buf), *f.number, f.decimals);
    } else if (!f.text[0]) {
        strcpy(buf, "(none)");
    } else if (f.kind == Field::PASSWORD) {
        size_t n = strlen(f.text);
        if (n > 12) n = 12;
        buf[0] = '\0';
        for (size_t i = 0; i < n; i++) strcat(buf, "*");
    } else {
        snprintf(buf, sizeof(buf), "%s", f.text);
    }
    setText(lv_obj_get_child(f.button, 0), buf);
}

void onNumberDone(float value, void *ctx) {
    Field &f = *(Field *)ctx;
    *f.number = value;
    showField(f);
    if (f.changed) f.changed(f);
}

void onTextDone(const char *text, void *ctx) {
    Field &f = *(Field *)ctx;
    strncpy(f.text, text, f.maxLen);
    f.text[f.maxLen] = '\0';
    showField(f);
    if (f.changed) f.changed(f);
}

void onFieldClick(lv_event_t *e) {
    Field &f = *(Field *)lv_event_get_user_data(e);
    if (f.kind == Field::NUMBER) {
        Entry::number(f.title, *f.number, f.min, f.max, f.decimals, f.unit, onNumberDone, &f);
    } else {
        Entry::text(f.title, f.text, f.maxLen, onTextDone, &f);
    }
}

Field &addField(lv_obj_t *parent, int x, int y, int w, int h, const char *title, const char *unit) {
    Field &f = fields[fieldCount++];
    memset(&f, 0, sizeof(f));
    f.title = title;
    f.unit = unit;
    f.button = button(parent, x, y, w, h, "", FONT_MID, onFieldClick, &f);
    lv_obj_set_style_border_width(f.button, 1, 0);
    lv_obj_set_style_border_color(f.button, UITheme::BORDER_LIGHT, 0);
    lv_label_set_long_mode(lv_obj_get_child(f.button, 0), LV_LABEL_LONG_DOT);
    lv_obj_set_width(lv_obj_get_child(f.button, 0), w - 16);
    return f;
}

Field &numberField(lv_obj_t *parent, int x, int y, int w, int h, const char *title, const char *unit, float *value,
                   float min, float max, int decimals) {
    Field &f = addField(parent, x, y, w, h, title, unit);
    f.kind = Field::NUMBER;
    f.number = value;
    f.min = min;
    f.max = max;
    f.decimals = decimals;
    return f;
}

void rowLabel(lv_obj_t *page, int y, const char *text, const char *unit, int width = VALUE_X - 12) {
    lv_obj_t *l = label(page, 0, y + 15, FONT_MID, UITheme::TEXT_LIGHT, text);
    lv_obj_set_width(l, width);
    if (unit) label(page, VALUE_X + VALUE_W + 12, y + 17, FONT_SMALL, UITheme::TEXT_MEDIUM, unit);
}

// A labelled number on one row
Field &numberRow(lv_obj_t *page, int y, const char *title, const char *unit, float *value, float min, float max,
                 int decimals) {
    rowLabel(page, y, title, unit);
    return numberField(page, VALUE_X, y + 2, VALUE_W, 52, title, unit, value, min, max, decimals);
}

// Text rows have a shorter label and a wider value
const int TEXT_X = 200, TEXT_W = CONTENT_W - TEXT_X;

Field &textRow(lv_obj_t *page, int y, const char *title, Field::Kind kind, size_t maxLen) {
    rowLabel(page, y, title, nullptr, TEXT_X - 12);
    Field &f = addField(page, TEXT_X, y + 2, TEXT_W, 52, title, nullptr);
    f.kind = kind;
    f.maxLen = maxLen;
    return f;
}

lv_obj_t *section(lv_obj_t *page, int y, const char *text) {
    return label(page, 0, y, FONT_BIG, UITheme::ACCENT_SECONDARY, text);
}

// --- Machine page ---------------------------------------------------------------

lv_obj_t *slotButtons[MAX_MACHINES];
lv_obj_t *machineNote;
bool machineLocked = false;  // a job runs

void showSlots() {
    for (int i = 0; i < MAX_MACHINES; i++) {
        const MachineConfig &m = machineDrafts[i];
        char buf[48];
        if (!slotConfigured[i] && !slotEdited[i]) {
            snprintf(buf, sizeof(buf), "%d  (empty)", i + 1);
        } else {
            snprintf(buf, sizeof(buf), "%d  %s%s", i + 1, m.name[0] ? m.name : "(no name)",
                     m.connection_type == CONN_WIFI ? "" : " (not WiFi)");
        }
        setText(lv_obj_get_child(slotButtons[i], 0), buf);
        setButtonColor(slotButtons[i], i == draftSlot ? UITheme::ACCENT_PRIMARY : UITheme::BG_BUTTON);
    }
}

void showMachine() {
    MachineConfig &m = machineDrafts[draftSlot];
    nameField->text = m.name;
    ssidField->text = m.ssid;
    passwordField->text = m.password;
    urlField->text = m.fluidnc_url;
    portDraft = m.websocket_port;
    for (Field *f : {nameField, ssidField, passwordField, urlField, portField}) showField(*f);
    showSlots();
}

void onMachineFieldChanged(Field &f) {
    MachineConfig &m = machineDrafts[draftSlot];
    if (&f == portField) m.websocket_port = (uint16_t)portDraft;
    slotEdited[draftSlot] = true;
    showSlots();
}

void onSlot(lv_event_t *e) {
    draftSlot = (int)(intptr_t)lv_event_get_user_data(e);
    showMachine();
}

void createMachinePage(lv_obj_t *page) {
    section(page, 0, "Machine in use");
    label(page, 170, 4, FONT_SMALL, UITheme::TEXT_MEDIUM, "Temporary, for development (D57)");
    for (int i = 0; i < MAX_MACHINES; i++) {
        slotButtons[i] = button(page, i * 148, 30, 140, 56, "", FONT_CHOICE, onSlot, (void *)(intptr_t)i);
        lv_obj_t *l = lv_obj_get_child(slotButtons[i], 0);
        lv_obj_set_width(l, 128);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    machineNote = label(page, 0, 96, FONT_SMALL, UITheme::TEXT_MEDIUM,
                        "Changes are saved when you leave Settings; a new address reconnects.");
    lv_obj_set_width(machineNote, CONTENT_W);

    int y = 120;
    nameField = &textRow(page, y, "Name", Field::TEXT, sizeof(MachineConfig::name) - 1);
    ssidField = &textRow(page, y += ROW_H, "WiFi network", Field::TEXT, sizeof(MachineConfig::ssid) - 1);
    passwordField = &textRow(page, y += ROW_H, "WiFi password", Field::PASSWORD, sizeof(MachineConfig::password) - 1);
    urlField = &textRow(page, y += ROW_H, "FluidNC address", Field::TEXT, sizeof(MachineConfig::fluidnc_url) - 1);
    rowLabel(page, y += ROW_H, "Port", nullptr, TEXT_X - 12);
    portField = &numberField(page, TEXT_X, y + 2, 140, 52, "Port", nullptr, &portDraft, 1, 65535, 0);
    for (Field *f : {nameField, ssidField, passwordField, urlField, portField}) f->changed = onMachineFieldChanged;
}

void setMachineLocked(bool locked) {
    if (locked == machineLocked) return;
    machineLocked = locked;
    for (lv_obj_t *b : slotButtons) setEnabled(b, !locked);
    for (Field *f : {nameField, ssidField, passwordField, urlField, portField}) setEnabled(f->button, !locked);
    setText(machineNote, locked ? "Not while a job runs."
                                : "Changes are saved when you leave Settings; a new address reconnects.");
    setTextColor(machineNote, locked ? UITheme::UI_WARNING : UITheme::TEXT_MEDIUM);
}

// --- Jog & Probe page ------------------------------------------------------------

// The step lists themselves are not changed here; only the step Setup starts
// with is chosen (D58)
lv_obj_t *xyStepButtons[STEPS], *zStepButtons[STEPS];
const int STEP_W = 112, STEP_GAP = 8;

void showStarts() {
    char text[12];
    for (int axis = 0; axis < 2; axis++) {
        lv_obj_t **buttons = axis ? zStepButtons : xyStepButtons;
        const float *steps = axis ? jogDraft.zSteps : jogDraft.xySteps;
        int start = axis ? jogDraft.zStart : jogDraft.xyStart;
        for (int i = 0; i < STEPS; i++) {
            if (steps[i] <= 0) {
                lv_obj_add_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_clear_flag(buttons[i], LV_OBJ_FLAG_HIDDEN);
            snprintf(text, sizeof(text), "%g", steps[i]);
            setText(lv_obj_get_child(buttons[i], 0), text);
            setButtonColor(buttons[i], i == start ? UITheme::ACCENT_PRIMARY : UITheme::BG_BUTTON);
        }
    }
}

int firstStep(const float *steps) {
    for (int i = 0; i < STEPS; i++) {
        if (steps[i] > 0) return i;
    }
    return 0;
}

void onStart(lv_event_t *e) {
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    if (code < STEPS) {
        jogDraft.xyStart = code;
    } else {
        jogDraft.zStart = code - STEPS;
    }
    showStarts();
}

void stepRow(lv_obj_t *page, int y, const char *title, lv_obj_t **buttons, int codeBase) {
    section(page, y, title);
    for (int i = 0; i < STEPS; i++) {
        buttons[i] = button(page, i * (STEP_W + STEP_GAP), y + 28, STEP_W, 52, "", FONT_MID, onStart,
                            (void *)(intptr_t)(codeBase + i));
    }
}

// Jog feeds: - and + change them by 100; the value itself opens the keypad (D58)
const int FEED_STEP = 100;
Field *feedFields[3];
int feedFieldCount = 0;

void onFeedStep(lv_event_t *e) {
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    Field &f = *feedFields[code / 2];
    float value = *f.number + (code % 2 ? FEED_STEP : -FEED_STEP);
    if (value < f.min || value > f.max) return;
    *f.number = value;
    showField(f);
}

Field &feedRow(lv_obj_t *page, int y, const char *title, float *value, float max) {
    rowLabel(page, y, title, nullptr, 224);
    int i = feedFieldCount++;
    button(page, VALUE_X - 64, y + 2, 56, 52, LV_SYMBOL_MINUS, FONT_MID, onFeedStep, (void *)(intptr_t)(i * 2));
    Field &f = numberField(page, VALUE_X, y + 2, VALUE_W, 52, title, "mm/min", value, 1, max, 0);
    button(page, VALUE_X + VALUE_W + 8, y + 2, 56, 52, LV_SYMBOL_PLUS, FONT_MID, onFeedStep,
           (void *)(intptr_t)(i * 2 + 1));
    feedFields[i] = &f;
    return f;
}

Field *jogFields[16];
int jogFieldCount = 0;

void createJogPage(lv_obj_t *page) {
    stepRow(page, 0, "XY step at start", xyStepButtons, 0);
    stepRow(page, 92, "Z step at start", zStepButtons, STEPS);

    int y = 184;
    section(page, y, "Jog feeds (mm/min)");
    y += 26;
    jogFields[jogFieldCount++] = &feedRow(page, y, "XY jog feed", &jogDraft.xyFeed, 20000);
    jogFields[jogFieldCount++] = &feedRow(page, y += ROW_H, "Z jog feed", &jogDraft.zFeed, 10000);
    jogFields[jogFieldCount++] = &feedRow(page, y += ROW_H, "Joystick full speed", &jogDraft.xyMaxFeed, 20000);

    section(page, y += ROW_H + 12, "Probe Z");
    y += 26;
    jogFields[jogFieldCount++] = &numberRow(page, y, "Probe feed", "mm/min", &probeFeed, 1, 2000, 0);
    jogFields[jogFieldCount++] = &numberRow(page, y += ROW_H, "Max distance", "mm", &probeDistance, 1, 200, 0);
    jogFields[jogFieldCount++] = &numberRow(page, y += ROW_H, "Back-off after touch", "mm", &probeBack, 0, 100, 1);

    section(page, y += ROW_H + 12, "Laser offset");
    label(page, 160, y + 4, FONT_SMALL, UITheme::TEXT_MEDIUM, "from the laser crosshair to the bit");
    y += 26;
    jogFields[jogFieldCount++] = &numberRow(page, y, "Laser offset X", "mm", &laserX, -500, 500, 2);
    jogFields[jogFieldCount++] = &numberRow(page, y += ROW_H, "Laser offset Y", "mm", &laserY, -500, 500, 2);
}

void showJogPage() {
    for (int i = 0; i < jogFieldCount; i++) showField(*jogFields[i]);
    showStarts();
}

// --- Display page ----------------------------------------------------------------

// One row of buttons choosing one value (classic's choices)
struct Choice {
    const char *title;
    const uint32_t *values;
    const char *const *names;
    int count;
    lv_obj_t *buttons[8];
};

const uint32_t DIM_VALUES[] = {0, 15, 30, 45, 60, 90, 120, 300};
const char *const DIM_NAMES[] = {"Off", "15 s", "30 s", "45 s", "1 min", "90 s", "2 min", "5 min"};
const uint32_t OFF_VALUES[] = {0, 60, 120, 300, 600, 900, 1800, 3600};
const char *const OFF_NAMES[] = {"Off", "1 min", "2 min", "5 min", "10 min", "15 min", "30 min", "60 min"};
const uint32_t DEEP_VALUES[] = {0, 300, 600, 900, 1800, 3600, 5400};
const char *const DEEP_NAMES[] = {"Off", "5 min", "10 min", "15 min", "30 min", "60 min", "90 min"};
const uint32_t ROTATE_VALUES[] = {0, 1};
const char *const ROTATE_NAMES[] = {"Off", "On"};

enum ChoiceId { CH_DIM, CH_OFF, CH_DEEP, CH_ROTATE, CH_COUNT };
Choice choices[CH_COUNT] = {
    {"Dim after", DIM_VALUES, DIM_NAMES, 8, {}},
    {"Screen off after", OFF_VALUES, OFF_NAMES, 8, {}},
    {"Deep sleep after", DEEP_VALUES, DEEP_NAMES, 7, {}},
    {"Rotate 180", ROTATE_VALUES, ROTATE_NAMES, 2, {}},
};

uint32_t choiceValue(int id) {
    const SettingsService::Display &d = displayDraft;
    switch (id) {
        case CH_DIM: return d.dimAfter;
        case CH_OFF: return d.offAfter;
        case CH_DEEP: return d.deepSleepAfter;
        default: return d.rotate180 ? 1 : 0;
    }
}

// Classic's rules: screen off at least 10 s after dim, deep sleep at least
// 60 s after screen off (0 = off, no rule)
bool offAllowed(uint32_t off) { return off == 0 || displayDraft.dimAfter == 0 || off >= displayDraft.dimAfter + 10; }
bool deepAllowed(uint32_t deep) { return deep == 0 || deep >= displayDraft.offAfter + 60; }

// A choice that breaks the rules moves the later timer to the first allowed value
void fixTimers() {
    if (!offAllowed(displayDraft.offAfter)) {
        for (uint32_t v : OFF_VALUES) {
            if (v && offAllowed(v)) {
                displayDraft.offAfter = v;
                break;
            }
        }
    }
    if (!deepAllowed(displayDraft.deepSleepAfter)) {
        displayDraft.deepSleepAfter = 0;
        for (uint32_t v : DEEP_VALUES) {
            if (v && deepAllowed(v)) {
                displayDraft.deepSleepAfter = v;
                break;
            }
        }
    }
}

void showChoices() {
    for (int id = 0; id < CH_COUNT; id++) {
        Choice &c = choices[id];
        uint32_t value = choiceValue(id);
        for (int i = 0; i < c.count; i++) {
            bool chosen = c.values[i] == value;
            setButtonColor(c.buttons[i], chosen ? UITheme::ACCENT_PRIMARY : UITheme::BG_BUTTON);
            bool allowed = id == CH_OFF ? offAllowed(c.values[i]) : id == CH_DEEP ? deepAllowed(c.values[i]) : true;
            setEnabled(c.buttons[i], allowed);
        }
    }
}

void onChoice(lv_event_t *e) {
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int id = code / 8, index = code % 8;
    uint32_t v = choices[id].values[index];
    SettingsService::Display &d = displayDraft;
    switch (id) {
        case CH_DIM: d.dimAfter = v; break;
        case CH_OFF: d.offAfter = v; break;
        case CH_DEEP: d.deepSleepAfter = v; break;
        default: d.rotate180 = v != 0; break;
    }
    fixTimers();
    showChoices();
}

// Brightness: a slider from 25 to 100% in steps of 5. Dimming goes to a
// quarter of it, at least 5% (D58).
const int BRIGHT_MIN = 25, BRIGHT_MAX = 100;
lv_obj_t *brightSlider;
lv_obj_t *brightValue;

uint8_t dimFor(int brightness) { return brightness / 4 < 5 ? 5 : brightness / 4; }

void showBrightness() {
    int b = displayDraft.brightness < BRIGHT_MIN ? BRIGHT_MIN : displayDraft.brightness;
    lv_slider_set_value(brightSlider, b, LV_ANIM_OFF);
    char buf[24];
    snprintf(buf, sizeof(buf), "%d%%", displayDraft.brightness);
    setText(brightValue, buf);
}

void onBrightness(lv_event_t *) {
    int v = (lv_slider_get_value(brightSlider) + 2) / 5 * 5;
    if (v < BRIGHT_MIN) v = BRIGHT_MIN;
    if (v > BRIGHT_MAX) v = BRIGHT_MAX;
    if (v == displayDraft.brightness) return;
    displayDraft.brightness = (uint8_t)v;
    displayDraft.dimBrightness = dimFor(v);
    PowerManager::setNormalBrightness(displayDraft.brightness);  // seen at once; stored on leaving
    showBrightness();
}

void createDisplayPage(lv_obj_t *page) {
    label(page, 0, 0, FONT_MID, UITheme::TEXT_LIGHT, "Brightness");
    label(page, 150, 2, FONT_SMALL, UITheme::TEXT_MEDIUM, "Dims to a quarter of this, at least 5%.");
    brightSlider = lv_slider_create(page);
    lv_obj_set_pos(brightSlider, 12, 46);
    lv_obj_set_size(brightSlider, 460, 14);
    lv_slider_set_range(brightSlider, BRIGHT_MIN, BRIGHT_MAX);
    lv_obj_set_style_bg_color(brightSlider, UITheme::BG_BUTTON, LV_PART_MAIN);
    lv_obj_set_style_bg_color(brightSlider, UITheme::ACCENT_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightSlider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightSlider, 10, LV_PART_KNOB);
    lv_obj_set_ext_click_area(brightSlider, 20);
    lv_obj_add_event_cb(brightSlider, onBrightness, LV_EVENT_VALUE_CHANGED, nullptr);
    brightValue = label(page, 500, 40, FONT_BIG, UITheme::TEXT_LIGHT, "");

    int y = 90;
    for (int id = 0; id < CH_COUNT; id++) {
        Choice &c = choices[id];
        lv_obj_t *t = label(page, 0, y, FONT_MID, UITheme::TEXT_LIGHT, c.title);
        if (id == CH_ROTATE) {
            lv_obj_t *note = label(page, 150, y + 2, FONT_SMALL, UITheme::TEXT_MEDIUM,
                                   "Upside-down mounting. Takes effect after a restart (Tools).");
            lv_obj_set_width(note, CONTENT_W - 150);
        }
        (void)t;
        int w = c.count > 4 ? 68 : 100, gap = c.count > 4 ? 6 : 8;
        for (int i = 0; i < c.count; i++) {
            c.buttons[i] = button(page, i * (w + gap), y + 26, w, 48, c.names[i], FONT_CHOICE, onChoice,
                                  (void *)(intptr_t)(id * 8 + i));
        }
        y += 90;
    }
}

// --- Tools page --------------------------------------------------------------------

lv_obj_t *aboutText;
uint32_t aboutAtMs = 0;

// Position log (D62): Off / On, and how the last recording went
bool posLogDraft = false;
lv_obj_t *posLogButtons[2];
lv_obj_t *posLogStatus;

void showPosLog() {
    setButtonColor(posLogButtons[0], posLogDraft ? UITheme::BG_BUTTON : UITheme::ACCENT_PRIMARY);
    setButtonColor(posLogButtons[1], posLogDraft ? UITheme::ACCENT_PRIMARY : UITheme::BG_BUTTON);
    char buf[256];
    const char *file = PositionLog::lastFile();
    const char *error = PositionLog::error();
    if (PositionLog::recording()) {
        snprintf(buf, sizeof(buf), "Recording to %s, %lu rows", file, (unsigned long)PositionLog::rows());
    } else if (file[0]) {
        snprintf(buf, sizeof(buf), "Last: %s, %lu rows%s%s", file, (unsigned long)PositionLog::rows(),
                 error[0] ? ". " : "", error);
    } else if (error[0]) {
        snprintf(buf, sizeof(buf), "Last job not recorded: %s", error);
    } else {
        snprintf(buf, sizeof(buf), "Records every job to /logs on the display SD.");
    }
    setText(posLogStatus, buf);
    setTextColor(posLogStatus, error[0] ? UITheme::STATE_ALARM : UITheme::TEXT_MEDIUM);
}

void onPosLog(lv_event_t *e) {
    posLogDraft = (intptr_t)lv_event_get_user_data(e) != 0;
    showPosLog();
}

void saveAll();

void onRestartClassic(lv_event_t *) {
    saveAll();
    UiMode::restartInto(UiMode::CLASSIC);
}

void onRestart(lv_event_t *) {
    saveAll();
    UiMode::restartInto(UiMode::APP);
}

void onDrawTest(lv_event_t *) { DrawTest::open(screenParent); }

void createToolsPage(lv_obj_t *page) {
    button(page, 0, 0, 280, 56, "Restart into classic UI", FONT_MID, onRestartClassic);
    button(page, 292, 0, 132, 56, "Restart", FONT_MID, onRestart);
    // Spike S2 (specs/06-test-and-spikes.md); goes away with the real preview
    button(page, 436, 0, 160, 56, "Draw test", FONT_MID, onDrawTest);

    section(page, 76, "Position log");
    posLogButtons[0] = button(page, 0, 106, 100, 52, "Off", FONT_MID, onPosLog, (void *)0);
    posLogButtons[1] = button(page, 108, 106, 100, 52, "On", FONT_MID, onPosLog, (void *)1);
    posLogStatus = label(page, 224, 122, FONT_SMALL, UITheme::TEXT_MEDIUM, "");
    lv_obj_set_width(posLogStatus, CONTENT_W - 224);
    lv_label_set_long_mode(posLogStatus, LV_LABEL_LONG_DOT);

    section(page, 178, "About");
    aboutText = label(page, 0, 208, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(aboutText, CONTENT_W);
    lv_obj_set_style_text_line_space(aboutText, 6, 0);
}

void showAbout() {
    char wifi[64];
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(wifi, sizeof(wifi), "%s, signal %d dBm", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    } else {
        strcpy(wifi, "not connected");
    }
    MachineConfig m;
    SettingsService::MachineStatus ms = SettingsService::machine(m);
    char controller[200];
    if (ms == SettingsService::MACHINE_NOT_CONFIGURED) {
        strcpy(controller, "none set up");
    } else {
        const char *version = MachineService::online() ? MachineService::status().fluidnc_version : "";
        snprintf(controller, sizeof(controller), "%s at %s:%u%s%s", m.name, m.fluidnc_url, (unsigned)m.websocket_port,
                 version[0] ? ", FluidNC " : "", version);
    }
    char buf[512];
    uint32_t up = millis() / 1000;
    snprintf(buf, sizeof(buf),
             "LowRider Touch (M3), FluidTouch " FLUIDTOUCH_VERSION ", built " __DATE__ "\n"
             "Machine %d: %s\n"
             "Display WiFi: %s\n"
             "Free RAM %u KB, PSRAM %u KB\n"
             "Running for %u:%02u:%02u",
             SettingsService::machineSlot() + 1, controller, wifi, (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(up / 3600), (unsigned)(up / 60 % 60),
             (unsigned)(up % 60));
    setText(aboutText, buf);
}

// --- Loading and saving ---------------------------------------------------------

void loadSteps(float *out, const float *steps, int count) {
    for (int i = 0; i < STEPS; i++) out[i] = i < count ? steps[i] : 0;
}

int nearest(const float *steps, float value) {
    int best = firstStep(steps);
    for (int i = 0; i < STEPS; i++) {
        if (steps[i] > 0 && fabsf(steps[i] - value) < fabsf(steps[best] - value)) best = i;
    }
    return best;
}

void loadDrafts() {
    SettingsService::Jog j = SettingsService::jog();
    loadSteps(jogDraft.xySteps, j.xySteps, j.xyStepCount);
    loadSteps(jogDraft.zSteps, j.zSteps, j.zStepCount);
    jogDraft.xyStart = nearest(jogDraft.xySteps, j.xyStep);
    jogDraft.zStart = nearest(jogDraft.zSteps, j.zStep);
    jogDraft.xyFeed = j.xyFeed;
    jogDraft.zFeed = j.zFeed;
    jogDraft.xyMaxFeed = j.xyMaxFeed;
    SettingsService::Probe p = SettingsService::probe();
    probeFeed = p.feed;
    probeDistance = p.maxDistance;
    probeBack = p.backOff;
    SettingsService::LaserOffset o = SettingsService::laserOffset();
    laserX = o.x;
    laserY = o.y;
    displayDraft = SettingsService::display();
    displayDraft.dimBrightness = dimFor(displayDraft.brightness);  // D58
    posLogDraft = SettingsService::positionLog();
}

void loadMachines() {
    for (int i = 0; i < MAX_MACHINES; i++) {
        slotConfigured[i] = SettingsService::machineInSlot(i, machineDrafts[i]);
        slotEdited[i] = false;
    }
    draftSlot = SettingsService::machineSlot();
}

void saveAll() {
    bool wrote = false;

    // Machine: the edited slots, then the slot in use
    bool reconnect = false;
    for (int i = 0; i < MAX_MACHINES; i++) {
        if (!slotEdited[i]) continue;
        bool changed = false;
        if (SettingsService::saveMachine(i, machineDrafts[i], changed)) wrote = true;
        reconnect |= changed;
        slotConfigured[i] = true;
        slotEdited[i] = false;
    }
    bool nameOnly = wrote && !reconnect;
    if (SettingsService::useSlot(draftSlot)) {
        wrote = true;
        reconnect = true;
    }
    if (reconnect || nameOnly) MachineService::useMachine(reconnect);

    SettingsService::Jog j = SettingsService::jog();
    j.xyStep = jogDraft.xySteps[jogDraft.xyStart];
    j.zStep = jogDraft.zSteps[jogDraft.zStart];
    j.xyFeed = (int)jogDraft.xyFeed;
    j.zFeed = (int)jogDraft.zFeed;
    j.xyMaxFeed = (int)jogDraft.xyMaxFeed;
    if (SettingsService::saveJog(j)) wrote = true;
    if (SettingsService::saveProbe({(int)probeFeed, (int)probeDistance, probeBack})) wrote = true;
    if (SettingsService::saveLaserOffset({laserX, laserY})) wrote = true;
    bool rotateChanged = displayDraft.rotate180 != SettingsService::display().rotate180;
    if (SettingsService::saveDisplay(displayDraft)) wrote = true;
    if (SettingsService::savePositionLog(posLogDraft)) wrote = true;

    if (wrote) {
        Frame::notice(rotateChanged ? "Settings saved. The rotation changes after a restart." : "Settings saved.");
    }
}

// --- Pages --------------------------------------------------------------------------

void showPage(int page) {
    pageShown = page;
    for (int i = 0; i < PAGE_COUNT; i++) {
        bool selected = i == page;
        if (selected) {
            lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        }
        setButtonColor(navButtons[i], selected ? UITheme::ACCENT_PRIMARY : UITheme::BG_BUTTON);
    }
    if (page == PAGE_TOOLS) {
        showAbout();
        showPosLog();
        aboutAtMs = millis();
    }
}

void onNav(lv_event_t *e) { showPage((int)(intptr_t)lv_event_get_user_data(e)); }

lv_obj_t *createPage(lv_obj_t *parent) {
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_pos(page, PAGE_X, 0);
    lv_obj_set_size(page, PAGE_W, Frame::CONTENT_HEIGHT);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_pad_top(page, 10, 0);
    lv_obj_set_style_pad_bottom(page, 16, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    return page;
}

}  // namespace

void SettingsScreen::create(lv_obj_t *parent) {
    screenParent = parent;
    for (int i = 0; i < PAGE_COUNT; i++) {
        navButtons[i] = button(parent, NAV_X, 8 + i * (NAV_H + NAV_GAP), NAV_W, NAV_H, PAGE_NAMES[i], FONT_MID, onNav,
                               (void *)(intptr_t)i);
        pages[i] = createPage(parent);
    }
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_pos(line, PAGE_X - 6, 8);
    lv_obj_set_size(line, 1, Frame::CONTENT_HEIGHT - 16);
    lv_obj_set_style_bg_color(line, UITheme::BORDER_MEDIUM, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

    createMachinePage(pages[PAGE_MACHINE]);
    createJogPage(pages[PAGE_JOG]);
    createDisplayPage(pages[PAGE_DISPLAY]);
    createToolsPage(pages[PAGE_TOOLS]);

    loadMachines();
    loadDrafts();
    showMachine();
    showJogPage();
    showChoices();
    showBrightness();
    showPage(PAGE_MACHINE);
}

void SettingsScreen::onShow() {
    // What was saved last time, closed up and as stored
    loadDrafts();
    showMachine();
    showJogPage();
    showChoices();
    showBrightness();
    showPage(pageShown);
}

void SettingsScreen::onHide() { saveAll(); }

void SettingsScreen::update() {
    setMachineLocked(MachineService::jobActive());
    if (pageShown == PAGE_TOOLS && millis() - aboutAtMs >= 1000) {
        aboutAtMs = millis();
        showAbout();
        showPosLog();
    }
}
