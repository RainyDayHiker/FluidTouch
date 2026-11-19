#include "app/entry.h"
#include "app/fonts.h"
#include "app/widgets.h"
#include "ui/ui_theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace Widgets;

namespace {

const lv_font_t *const FONT_TITLE = &lrt_font_20;
const lv_font_t *const FONT_VALUE = &lrt_font_32;
const lv_font_t *const FONT_KEY = &lrt_font_26;
const lv_font_t *const FONT_MID = &lrt_font_18;
const lv_font_t *const FONT_SMALL = &lrt_font_14;

// --- Keypad ------------------------------------------------------------------

const int PAD_W = 440, PAD_H = 448;
const int KEY_W = 96, KEY_H = 70, KEY_GAP = 8, KEYS_X = 16, KEYS_Y = 136;

lv_obj_t *pad;
lv_obj_t *padTitle;
lv_obj_t *padValue;
lv_obj_t *padRange;
lv_obj_t *minusKey;
lv_obj_t *pointKey;
char padText[16];
bool padFresh;  // the first key replaces the value shown at the start
float padMin, padMax;
int padDecimals;
Entry::NumberDone padDone;
void *padCtx;

void showPadText() { setText(padValue, padText[0] ? padText : "0"); }

void showRange(bool refused) {
    char a[16], b[16], buf[64];
    Entry::formatNumber(a, sizeof(a), padMin, padDecimals);
    Entry::formatNumber(b, sizeof(b), padMax, padDecimals);
    snprintf(buf, sizeof(buf), "%s to %s", a, b);
    setText(padRange, buf);
    setTextColor(padRange, refused ? UITheme::STATE_ALARM : UITheme::TEXT_MEDIUM);
}

void onPadKey(lv_event_t *e) {
    char key = (char)(intptr_t)lv_event_get_user_data(e);
    size_t len = strlen(padText);
    if (padFresh && key != '<') {
        padText[0] = '\0';
        len = 0;
    }
    padFresh = false;
    if (key == '<') {
        if (len) padText[len - 1] = '\0';
    } else if (key == '-') {
        // Toggles the sign
        if (padText[0] == '-') {
            memmove(padText, padText + 1, len);
        } else if (len + 1 < sizeof(padText)) {
            memmove(padText + 1, padText, len + 1);
            padText[0] = '-';
        }
    } else if (key == '.') {
        if (!strchr(padText, '.') && len + 2 < sizeof(padText)) {
            if (len == 0 || (len == 1 && padText[0] == '-')) strcat(padText, "0");
            strcat(padText, ".");
        }
    } else if (len + 1 < sizeof(padText)) {
        // At most padDecimals after the point
        const char *point = strchr(padText, '.');
        if (!point || (int)strlen(point + 1) < padDecimals) {
            padText[len] = key;
            padText[len + 1] = '\0';
        }
    }
    showPadText();
    showRange(false);
}

void onPadCancel(lv_event_t *) { showDialog(pad, false); }

void onPadOk(lv_event_t *) {
    char *end;
    float value = strtof(padText, &end);
    bool valid = padText[0] && end != padText && *end == '\0';
    // A tolerance, so 0.1 typed is not refused against a 0.1 minimum
    float slack = 0.5f * powf(10.0f, (float)-padDecimals);
    if (!valid || value < padMin - slack || value > padMax + slack) {
        showRange(true);
        return;
    }
    showDialog(pad, false);
    if (padDone) padDone(value, padCtx);
}

void createPad() {
    pad = dialog(lv_layer_top(), PAD_W, PAD_H);
    padTitle = label(pad, 16, 12, FONT_TITLE, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(padTitle, PAD_W - 32);
    lv_label_set_long_mode(padTitle, LV_LABEL_LONG_DOT);

    lv_obj_t *box = lv_obj_create(pad);
    lv_obj_set_pos(box, KEYS_X, 44);
    lv_obj_set_size(box, PAD_W - 2 * KEYS_X, 60);
    lv_obj_set_style_bg_color(box, UITheme::BG_DARKER, 0);
    lv_obj_set_style_border_color(box, UITheme::ACCENT_PRIMARY, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_hor(box, 12, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    padValue = lv_label_create(box);
    lv_obj_set_style_text_font(padValue, FONT_VALUE, 0);
    lv_obj_set_style_text_color(padValue, lv_color_white(), 0);
    lv_obj_set_width(padValue, lv_pct(100));
    lv_obj_set_style_text_align(padValue, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(padValue, LV_ALIGN_RIGHT_MID, 0, 0);

    padRange = label(pad, 16, 110, FONT_SMALL, UITheme::TEXT_MEDIUM, "");

    static const char keys[4][4] = {{'7', '8', '9', '<'}, {'4', '5', '6', '-'}, {'1', '2', '3', '.'}, {'0', 0, 0, 0}};
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            char k = keys[row][col];
            if (!k) continue;
            char text[8] = {k, 0};
            if (k == '<') strcpy(text, LV_SYMBOL_BACKSPACE);
            lv_obj_t *b = button(pad, KEYS_X + col * (KEY_W + KEY_GAP), KEYS_Y + row * (KEY_H + KEY_GAP), KEY_W,
                                 KEY_H, text, FONT_KEY, onPadKey, (void *)(intptr_t)k);
            if (k == '-') minusKey = b;
            if (k == '.') pointKey = b;
        }
    }
    int y = KEYS_Y + 3 * (KEY_H + KEY_GAP);
    button(pad, KEYS_X + (KEY_W + KEY_GAP), y, KEY_W, KEY_H, "Cancel", FONT_MID, onPadCancel);
    lv_obj_t *ok = button(pad, KEYS_X + 2 * (KEY_W + KEY_GAP), y, 2 * KEY_W + KEY_GAP, KEY_H, "OK", FONT_KEY, onPadOk);
    setButtonColor(ok, UITheme::ACCENT_PRIMARY);
}

// --- Keyboard ------------------------------------------------------------------

lv_obj_t *board;
lv_obj_t *boardTitle;
lv_obj_t *boardField;
lv_obj_t *boardKeys;
Entry::TextDone boardDone;
void *boardCtx;

void onBoardEvent(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;
    showDialog(board, false);
    if (code == LV_EVENT_READY && boardDone) boardDone(lv_textarea_get_text(boardField), boardCtx);
}

void createBoard() {
    board = dialog(lv_layer_top(), 792, 472);
    boardTitle = label(board, 16, 10, FONT_TITLE, UITheme::TEXT_LIGHT, "");
    lv_obj_t *hint = label(board, 440, 14, FONT_SMALL, UITheme::TEXT_MEDIUM,
                           LV_SYMBOL_OK " to keep, " LV_SYMBOL_KEYBOARD " to cancel");
    lv_obj_set_width(hint, 336);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_RIGHT, 0);

    boardField = lv_textarea_create(board);
    lv_obj_set_pos(boardField, 16, 42);
    lv_obj_set_size(boardField, 760, 56);
    lv_textarea_set_one_line(boardField, true);
    lv_obj_set_style_text_font(boardField, FONT_TITLE, 0);
    lv_obj_set_style_bg_color(boardField, UITheme::BG_DARKER, 0);
    lv_obj_set_style_text_color(boardField, lv_color_white(), 0);
    lv_obj_set_style_border_color(boardField, UITheme::ACCENT_PRIMARY, 0);
    lv_obj_add_state(boardField, LV_STATE_FOCUSED);  // shows the cursor

    boardKeys = lv_keyboard_create(board);
    // A keyboard keeps its own bottom alignment, so it is aligned, not placed
    lv_obj_set_size(boardKeys, 790, 362);
    lv_obj_align(boardKeys, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(boardKeys, FONT_MID, 0);
    lv_keyboard_set_textarea(boardKeys, boardField);
    lv_obj_add_event_cb(boardKeys, onBoardEvent, LV_EVENT_ALL, nullptr);
}

}  // namespace

namespace Entry {

void formatNumber(char *out, size_t outLen, float value, int decimals) {
    snprintf(out, outLen, "%.*f", decimals, value);
    // 12.500 -> 12.5, 3.000 -> 3
    if (decimals > 0) {
        char *p = out + strlen(out) - 1;
        while (p > out && *p == '0') *p-- = '\0';
        if (*p == '.') *p = '\0';
    }
    if (strcmp(out, "-0") == 0) strcpy(out, "0");
}

void number(const char *title, float value, float min, float max, int decimals, const char *unit,
            NumberDone done, void *ctx) {
    if (!pad) createPad();
    char buf[96];
    snprintf(buf, sizeof(buf), unit && unit[0] ? "%s (%s)" : "%s", title, unit);
    setText(padTitle, buf);
    padMin = min;
    padMax = max;
    padDecimals = decimals;
    padDone = done;
    padCtx = ctx;
    formatNumber(padText, sizeof(padText), value, decimals);
    padFresh = true;
    // Keys that can't be used here are left out
    if (min < 0) lv_obj_clear_flag(minusKey, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(minusKey, LV_OBJ_FLAG_HIDDEN);
    if (decimals > 0) lv_obj_clear_flag(pointKey, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(pointKey, LV_OBJ_FLAG_HIDDEN);
    showPadText();
    showRange(false);
    showDialog(pad, true);
}

void text(const char *title, const char *value, size_t maxLen, TextDone done, void *ctx) {
    if (!board) createBoard();
    setText(boardTitle, title);
    lv_textarea_set_max_length(boardField, maxLen);
    lv_textarea_set_text(boardField, value);
    lv_keyboard_set_mode(boardKeys, LV_KEYBOARD_MODE_TEXT_LOWER);
    boardDone = done;
    boardCtx = ctx;
    showDialog(board, true);
}

}  // namespace Entry
