#include "app/screens/screens.h"
#include "app/fonts.h"
#include "app/entry.h"
#include "app/frame.h"
#include "app/widgets.h"
#include "services/file_service.h"
#include "services/machine_service.h"
#include "services/macro_service.h"
#include "ui/ui_theme.h"
#include <Arduino.h>
#include <string.h>

// The Macros screen (specs/03-screens.md section 5, D59 to D61). Content area
// 800 x 360:
//
//   y   0..56   strip: a hint and Edit, or the running macro with Resume and Stop
//   y  64..344  4 x 3 grid of macro buttons
//
// Edit mode: tapping a slot opens the edit pop-up; its file button opens the
// list of files in the macros folder. Both cover the content area.

using namespace Widgets;

namespace {

const lv_font_t *const FONT_LARGE = &lrt_font_24;
const lv_font_t *const FONT_BIG = &lrt_font_20;
const lv_font_t *const FONT_MID = &lrt_font_18;
const lv_font_t *const FONT_SMALL = &lrt_font_14;

const int SLOTS = MacroService::SLOTS;
const int COLS = 4;
const int CELL_W = 190, CELL_H = 88, GAP = 8, GRID_X = 8, GRID_Y = 64;
const int STRIP_H = 56;
const int ROW_H = 52;

lv_color_t macroColor(int i) {
    static const lv_color_t colors[MacroService::COLORS] = {
        UITheme::MACRO_COLOR_1, UITheme::MACRO_COLOR_2, UITheme::MACRO_COLOR_3, UITheme::MACRO_COLOR_4,
        UITheme::MACRO_COLOR_5, UITheme::MACRO_COLOR_6, UITheme::MACRO_COLOR_7, UITheme::MACRO_COLOR_8,
    };
    return colors[i < 0 || i >= MacroService::COLORS ? 0 : i];
}

bool editing = false;
bool gridDirty = true;

// Strip
lv_obj_t *hintGroup;
lv_obj_t *hintLabel;
lv_obj_t *editButton;
lv_obj_t *runGroup;
lv_obj_t *runName;
lv_obj_t *runMessage;
lv_obj_t *runBar;
lv_obj_t *runPercent;
lv_obj_t *resumeButton;
lv_obj_t *stopButton;

// Grid
lv_obj_t *cells[SLOTS];
lv_obj_t *cellNames[SLOTS];
lv_obj_t *cellDetails[SLOTS];

// Edit pop-up
lv_obj_t *editPanel;
lv_obj_t *editTitle;
lv_obj_t *nameButton;
lv_obj_t *kindButtons[2];
lv_obj_t *valueTitle;
lv_obj_t *valueButton;
lv_obj_t *swatches[MacroService::COLORS];
lv_obj_t *leftButton;
lv_obj_t *rightButton;
int editSlot = -1;

// File list pop-up
lv_obj_t *filePanel;
lv_obj_t *fileList;
bool filesWaiting = false;
uint32_t filesGeneration = 0;

void show(lv_obj_t *obj, bool visible) {
    if (visible == lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        if (visible) {
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

lv_obj_t *group(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_pos(g, x, y);
    lv_obj_set_size(g, w, h);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    return g;
}

// Left-aligned, cut off with dots, inside a button
void leftLabel(lv_obj_t *btn, int width) {
    lv_obj_t *l = lv_obj_get_child(btn, 0);
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 14, 0);
}

// --- Grid ---------------------------------------------------------------------

void showGrid() {
    gridDirty = false;
    for (int i = 0; i < SLOTS; i++) {
        const MacroService::Macro &m = MacroService::get(i);
        bool empty = m.kind == MacroService::KIND_EMPTY;
        show(cells[i], editing || !empty);
        if (empty) {
            setButtonColor(cells[i], UITheme::BG_BUTTON);
            setText(cellNames[i], LV_SYMBOL_PLUS);
            setText(cellDetails[i], "");
            lv_obj_align(cellNames[i], LV_ALIGN_CENTER, 0, 0);
            continue;
        }
        setButtonColor(cells[i], macroColor(m.color));
        setText(cellNames[i], m.name[0] ? m.name : m.text[0] ? m.text : "(no name)");
        const char *detail = "";
        if (!m.text[0]) {
            detail = m.kind == MacroService::KIND_FILE ? "no file chosen" : "no command";
        } else if (editing) {
            detail = m.text;
        }
        setText(cellDetails[i], detail);
        // The name in the middle, or above the detail line
        lv_obj_align(cellNames[i], LV_ALIGN_CENTER, 0, detail[0] ? -8 : 0);
    }
}

void updateGrid() {
    if (gridDirty) showGrid();
    bool canRun = MacroService::canRun();
    for (int i = 0; i < SLOTS; i++) {
        const MacroService::Macro &m = MacroService::get(i);
        setEnabled(cells[i], editing || (canRun && m.kind != MacroService::KIND_EMPTY && m.text[0]));
    }
}

void openEditor(int slot);

void onCellClicked(lv_event_t *e) {
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (editing) {
        openEditor(slot);
        return;
    }
    if (!MacroService::run(slot)) {
        const char *why = MacroService::active() ? "Wait for the macro to finish." : MachineService::busyReason();
        if (why && why[0]) Frame::notice(why);
    }
}

// --- Strip --------------------------------------------------------------------

void leaveEditing() {
    if (!editing) return;
    editing = false;
    gridDirty = true;
    setText(lv_obj_get_child(editButton, 0), LV_SYMBOL_EDIT "  Edit");
    if (MacroService::save()) Frame::notice("Macros saved.");
}

void onEditClicked(lv_event_t *) {
    if (editing) {
        leaveEditing();
        return;
    }
    editing = true;
    gridDirty = true;
    setText(lv_obj_get_child(editButton, 0), LV_SYMBOL_OK "  Done");
}

void onResumeClicked(lv_event_t *) { MacroService::resume(); }
void onStopClicked(lv_event_t *) { MacroService::stop(); }

void updateStrip() {
    MacroService::RunState st = MacroService::runState();
    bool running = !editing && st != MacroService::RUN_NONE;
    show(hintGroup, !running);
    show(runGroup, running);
    // Edit (or Done) whenever Stop is not there
    show(editButton, editing || !MacroService::active());
    if (!running) {
        const char *hint;
        if (editing) {
            hint = "Tap a slot to change it.";
        } else if (MacroService::count() == 0) {
            hint = "No macros yet. Tap Edit to add one.";
        } else {
            hint = "Tap a macro to run it.";
        }
        setText(hintLabel, hint);
        return;
    }

    char text[MacroService::NAME_LEN + 32];
    const char *what = "";
    switch (st) {
        case MacroService::RUN_STARTING: what = "starting"; break;
        case MacroService::RUN_RUNNING: what = "running"; break;
        case MacroService::RUN_HELD: what = "held"; break;
        case MacroService::RUN_DONE: what = "done"; break;
        case MacroService::RUN_STOPPED: what = "stopped"; break;
        default: break;
    }
    snprintf(text, sizeof(text), "%s: %s", MacroService::runName(), what);
    setText(runName, text);
    setTextColor(runName, st == MacroService::RUN_STOPPED ? UITheme::STATE_ALARM
                          : st == MacroService::RUN_DONE  ? UITheme::UI_SUCCESS
                                                          : UITheme::TEXT_LIGHT);

    const char *message = st == MacroService::RUN_STOPPED ? MacroService::endText()
                                                          : MacroService::message();
    setText(runMessage, message);

    bool bar = MacroService::runIsFile() && st != MacroService::RUN_STOPPED;
    show(runBar, bar);
    show(runPercent, bar);
    if (bar) {
        int p = (int)MacroService::percent();
        lv_bar_set_value(runBar, p, LV_ANIM_OFF);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", p);
        setText(runPercent, buf);
    }
    show(resumeButton, st == MacroService::RUN_HELD);
    show(stopButton, MacroService::active());
}

// --- File list pop-up ----------------------------------------------------------

void openEditorAgain();

void onFileClicked(lv_event_t *e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (editSlot < 0 || index >= FileService::count()) return;
    const FileService::Entry &entry = FileService::entry(index);
    MacroService::Macro m = MacroService::get(editSlot);
    strncpy(m.text, entry.name, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = '\0';
    if (!m.name[0]) {
        // The file name without its extension
        strncpy(m.name, entry.name, sizeof(m.name) - 1);
        m.name[sizeof(m.name) - 1] = '\0';
        char *dot = strrchr(m.name, '.');
        if (dot && dot != m.name) *dot = '\0';
    }
    MacroService::set(editSlot, m);
    gridDirty = true;
    showDialog(filePanel, false);
    openEditorAgain();
}

void fileStatus(const char *text, lv_color_t color) {
    lv_obj_clean(fileList);
    label(fileList, 0, 0, FONT_MID, color, text);
}

void showFiles() {
    filesWaiting = false;
    filesGeneration = FileService::listGeneration();
    if (FileService::listState() == FileService::LIST_FAILED) {
        fileStatus(FileService::listError(), UITheme::STATE_ALARM);
        return;
    }
    lv_obj_clean(fileList);
    int shown = 0;
    for (int i = 0; i < FileService::count(); i++) {
        const FileService::Entry &entry = FileService::entry(i);
        if (entry.dir) continue;
        lv_obj_t *b = button(fileList, 0, shown * (ROW_H + 6), 588, ROW_H, entry.name, FONT_MID, onFileClicked,
                             (void *)(intptr_t)i);
        leftLabel(b, 560);
        shown++;
    }
    if (!shown) fileStatus("No files in the macros folder.", UITheme::TEXT_MEDIUM);
}

void onFileButtonClicked(lv_event_t *) {
    showDialog(filePanel, true);
    if (!MachineService::online()) {
        fileStatus("Not connected to the controller.", UITheme::TEXT_MEDIUM);
        return;
    }
    fileStatus("Loading...", UITheme::TEXT_MEDIUM);
    FileService::list(FileService::CARD_CONTROLLER, MacroService::FOLDER);
    filesWaiting = true;
}

void onFileCloseClicked(lv_event_t *) {
    filesWaiting = false;
    showDialog(filePanel, false);
}

void updateFiles() {
    if (!filesWaiting) return;
    if (FileService::listGeneration() != filesGeneration && FileService::listCard() == FileService::CARD_CONTROLLER &&
        (FileService::listState() == FileService::LIST_READY || FileService::listState() == FileService::LIST_FAILED)) {
        showFiles();
    }
}

// --- Edit pop-up ----------------------------------------------------------------

void showEditor() {
    const MacroService::Macro &m = MacroService::get(editSlot);
    char text[24];
    snprintf(text, sizeof(text), "Slot %d", editSlot + 1);
    setText(editTitle, text);
    setText(lv_obj_get_child(nameButton, 0), m.name[0] ? m.name : "(tap to name it)");
    bool isFile = m.kind != MacroService::KIND_COMMAND;
    setButtonColor(kindButtons[0], isFile ? UITheme::ACCENT_PRIMARY : UITheme::BG_BUTTON);
    setButtonColor(kindButtons[1], isFile ? UITheme::BG_BUTTON : UITheme::ACCENT_PRIMARY);
    setText(valueTitle, isFile ? "File" : "Command");
    const char *value = m.text;
    if (!value[0]) value = isFile ? "(tap to choose a file)" : "(tap to type the command)";
    setText(lv_obj_get_child(valueButton, 0), value);
    for (int i = 0; i < MacroService::COLORS; i++) {
        lv_obj_set_style_border_width(swatches[i], i == m.color ? 4 : 0, 0);
        setText(lv_obj_get_child(swatches[i], 0), i == m.color ? LV_SYMBOL_OK : "");
    }
    setEnabled(leftButton, editSlot > 0);
    setEnabled(rightButton, editSlot < SLOTS - 1);
}

void openEditor(int slot) {
    editSlot = slot;
    MacroService::Macro m = MacroService::get(slot);
    if (m.kind == MacroService::KIND_EMPTY) {
        m.kind = MacroService::KIND_FILE;  // a new slot starts as a file macro
        MacroService::set(slot, m);
        gridDirty = true;
    }
    showEditor();
    showDialog(editPanel, true);
}

void openEditorAgain() {
    if (editSlot >= 0) showEditor();
}

// A slot left with no name and no file or command is empty again
void onEditCloseClicked(lv_event_t *) {
    if (editSlot >= 0) {
        const MacroService::Macro &m = MacroService::get(editSlot);
        if (!m.name[0] && !m.text[0]) MacroService::clear(editSlot);
    }
    editSlot = -1;
    gridDirty = true;
    showDialog(editPanel, false);
}

void onNameDone(const char *text, void *) {
    if (editSlot < 0) return;
    MacroService::Macro m = MacroService::get(editSlot);
    strncpy(m.name, text, sizeof(m.name) - 1);
    m.name[sizeof(m.name) - 1] = '\0';
    MacroService::set(editSlot, m);
    gridDirty = true;
    showEditor();
}

void onNameClicked(lv_event_t *) {
    Entry::text("Macro name", MacroService::get(editSlot).name, MacroService::NAME_LEN - 1, onNameDone, nullptr);
}

void onKindClicked(lv_event_t *e) {
    uint8_t kind = (intptr_t)lv_event_get_user_data(e) ? MacroService::KIND_COMMAND : MacroService::KIND_FILE;
    MacroService::Macro m = MacroService::get(editSlot);
    if (m.kind == kind) return;
    m.kind = kind;
    m.text[0] = '\0';  // a file name is no command, and the other way round
    MacroService::set(editSlot, m);
    gridDirty = true;
    showEditor();
}

void onCommandDone(const char *text, void *) {
    if (editSlot < 0) return;
    MacroService::Macro m = MacroService::get(editSlot);
    // Leading and trailing spaces don't belong to the command
    while (*text == ' ') text++;
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = '\0';
    for (int n = (int)strlen(m.text); n > 0 && m.text[n - 1] == ' '; n--) m.text[n - 1] = '\0';
    MacroService::set(editSlot, m);
    gridDirty = true;
    showEditor();
}

void onValueClicked(lv_event_t *e) {
    if (MacroService::get(editSlot).kind == MacroService::KIND_COMMAND) {
        Entry::text("Command", MacroService::get(editSlot).text, MacroService::TEXT_LEN - 1, onCommandDone, nullptr);
    } else {
        onFileButtonClicked(e);
    }
}

void onSwatchClicked(lv_event_t *e) {
    MacroService::Macro m = MacroService::get(editSlot);
    m.color = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    MacroService::set(editSlot, m);
    gridDirty = true;
    showEditor();
}

void onMoveClicked(lv_event_t *e) {
    int to = editSlot + ((intptr_t)lv_event_get_user_data(e) ? 1 : -1);
    if (to < 0 || to >= SLOTS) return;
    MacroService::swap(editSlot, to);
    editSlot = to;
    gridDirty = true;
    showEditor();
}

void onClearClicked(lv_event_t *) {
    MacroService::clear(editSlot);
    editSlot = -1;
    gridDirty = true;
    showDialog(editPanel, false);
}

void createEditor(lv_obj_t *parent) {
    const int W = 660, X = 150, VW = 490;
    editPanel = dialog(parent, W, 344);
    editTitle = label(editPanel, 16, 12, FONT_BIG, UITheme::TEXT_LIGHT, "");

    label(editPanel, 16, 62, FONT_MID, UITheme::TEXT_MEDIUM, "Name");
    nameButton = button(editPanel, X, 48, VW, ROW_H, "", FONT_MID, onNameClicked);
    leftLabel(nameButton, VW - 28);

    label(editPanel, 16, 122, FONT_MID, UITheme::TEXT_MEDIUM, "Runs");
    kindButtons[0] = button(editPanel, X, 108, 160, ROW_H, "File", FONT_MID, onKindClicked, (void *)0);
    kindButtons[1] = button(editPanel, X + 168, 108, 160, ROW_H, "Command", FONT_MID, onKindClicked, (void *)1);

    valueTitle = label(editPanel, 16, 182, FONT_MID, UITheme::TEXT_MEDIUM, "");
    valueButton = button(editPanel, X, 168, VW, ROW_H, "", FONT_MID, onValueClicked);
    leftLabel(valueButton, VW - 28);

    label(editPanel, 16, 240, FONT_MID, UITheme::TEXT_MEDIUM, "Colour");
    for (int i = 0; i < MacroService::COLORS; i++) {
        swatches[i] = button(editPanel, X + i * 62, 228, 52, 44, "", FONT_MID, onSwatchClicked, (void *)(intptr_t)i);
        setButtonColor(swatches[i], macroColor(i));
        lv_obj_set_style_border_color(swatches[i], lv_color_white(), 0);
    }

    const int BY = 284, BW = 148;
    leftButton = button(editPanel, 16, BY, BW, ROW_H, LV_SYMBOL_LEFT "  Move", FONT_MID, onMoveClicked, (void *)0);
    rightButton = button(editPanel, 16 + BW + 8, BY, BW, ROW_H, "Move  " LV_SYMBOL_RIGHT, FONT_MID, onMoveClicked,
                         (void *)1);
    lv_obj_t *clearButton = button(editPanel, W - 16 - 2 * BW - 8, BY, BW, ROW_H, LV_SYMBOL_TRASH "  Clear",
                                   FONT_MID, onClearClicked);
    setButtonColor(clearButton, LV_COLOR_MAKE(0x8B, 0x1E, 0x1E));
    lv_obj_t *closeButton = button(editPanel, W - 16 - BW, BY, BW, ROW_H, "Close", FONT_MID, onEditCloseClicked);
    setButtonColor(closeButton, UITheme::ACCENT_PRIMARY);
}

void createFileList(lv_obj_t *parent) {
    const int W = 660;
    filePanel = dialog(parent, W, 344);
    label(filePanel, 16, 14, FONT_BIG, UITheme::TEXT_LIGHT, "Files in /sd/fluidtouch/macros");
    lv_obj_t *close = button(filePanel, W - 16 - 120, 8, 120, 48, "Close", FONT_MID, onFileCloseClicked);
    setButtonColor(close, UITheme::ACCENT_PRIMARY);
    fileList = lv_obj_create(filePanel);
    lv_obj_set_pos(fileList, 16, 68);
    lv_obj_set_size(fileList, W - 32, 344 - 68 - 12);
    lv_obj_set_style_bg_opa(fileList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fileList, 0, 0);
    lv_obj_set_style_pad_all(fileList, 0, 0);
    lv_obj_set_scroll_dir(fileList, LV_DIR_VER);
}

}  // namespace

void MacrosScreen::create(lv_obj_t *parent) {
    // Strip
    lv_obj_t *strip = group(parent, 8, 0, 784, STRIP_H);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(strip, UITheme::BG_DARKER, 0);
    lv_obj_set_style_radius(strip, 6, 0);

    hintGroup = group(strip, 0, 0, 784, STRIP_H);
    hintLabel = label(hintGroup, 14, 16, FONT_MID, UITheme::TEXT_MEDIUM, "");
    // On the strip itself: also shown beside "done" or "stopped"
    editButton = button(strip, 784 - 4 - 140, 4, 140, 48, LV_SYMBOL_EDIT "  Edit", FONT_MID, onEditClicked);
    setButtonColor(editButton, UITheme::ACCENT_SECONDARY);

    runGroup = group(strip, 0, 0, 784, STRIP_H);
    runName = label(runGroup, 12, 4, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(runName, 300);
    lv_label_set_long_mode(runName, LV_LABEL_LONG_DOT);
    runMessage = label(runGroup, 12, 30, FONT_SMALL, UITheme::UI_INFO, "");
    lv_obj_set_width(runMessage, 500);
    lv_label_set_long_mode(runMessage, LV_LABEL_LONG_DOT);
    runBar = lv_bar_create(runGroup);
    lv_obj_set_pos(runBar, 320, 10);
    lv_obj_set_size(runBar, 140, 12);
    lv_bar_set_range(runBar, 0, 100);
    lv_obj_set_style_bg_color(runBar, UITheme::BG_BLACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(runBar, UITheme::UI_SUCCESS, LV_PART_INDICATOR);
    runPercent = label(runGroup, 468, 6, FONT_SMALL, UITheme::UI_SUCCESS, "");
    resumeButton = button(runGroup, 784 - 4 - 120 - 8 - 120, 4, 120, 48, LV_SYMBOL_PLAY " Resume", FONT_MID,
                          onResumeClicked);
    setButtonColor(resumeButton, UITheme::BTN_PLAY);
    stopButton = button(runGroup, 784 - 4 - 120, 4, 120, 48, LV_SYMBOL_STOP " Stop", FONT_MID, onStopClicked);
    setButtonColor(stopButton, UITheme::STATE_ALARM);
    lv_obj_add_flag(runGroup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(editButton);  // above runGroup, which covers the strip

    // Grid
    for (int i = 0; i < SLOTS; i++) {
        int x = GRID_X + (i % COLS) * (CELL_W + GAP);
        int y = GRID_Y + (i / COLS) * (CELL_H + GAP);
        cells[i] = button(parent, x, y, CELL_W, CELL_H, "", FONT_LARGE, onCellClicked, (void *)(intptr_t)i);
        cellNames[i] = lv_obj_get_child(cells[i], 0);
        lv_obj_set_width(cellNames[i], CELL_W - 16);
        lv_label_set_long_mode(cellNames[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(cellNames[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(cellNames[i], LV_ALIGN_CENTER, 0, -8);
        cellDetails[i] = lv_label_create(cells[i]);
        lv_obj_set_width(cellDetails[i], CELL_W - 16);
        lv_label_set_long_mode(cellDetails[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(cellDetails[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(cellDetails[i], FONT_SMALL, 0);
        lv_obj_align(cellDetails[i], LV_ALIGN_CENTER, 0, 22);
        lv_label_set_text(cellDetails[i], "");
    }

    createEditor(parent);
    createFileList(parent);
    showGrid();
}

void MacrosScreen::update() {
    updateStrip();
    updateGrid();
    updateFiles();
}

void MacrosScreen::onHide() {
    if (editSlot >= 0) onEditCloseClicked(nullptr);
    showDialog(filePanel, false);
    filesWaiting = false;
    leaveEditing();
}
