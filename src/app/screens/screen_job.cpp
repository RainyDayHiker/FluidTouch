#include "app/screens/screens.h"
#include "app/fonts.h"
#include "app/frame.h"
#include "app/preview_canvas.h"
#include "app/widgets.h"
#include "services/file_service.h"
#include "services/job_service.h"
#include "services/machine_service.h"
#include "services/preview_service.h"
#include "ui/ui_theme.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>

// The Job screen (specs/03-screens.md section 3). Content area 800 x 360:
//
//   y   8..72   the chosen file; tapping it opens the file picker
//   y  80..284  the preview (specs/11-preview.md §4): the job drawn, with Fit in its corner; or
//               the download and analysis with their bar, the upload, or why there is none
//   y 296..352  the info line and warnings (§5), Use this job
//
// The picker covers the whole content area while open.

using namespace Widgets;

namespace {

const lv_font_t *const FONT_LARGE = &lrt_font_24;
const lv_font_t *const FONT_BIG = &lrt_font_20;
const lv_font_t *const FONT_MID = &lrt_font_18;
const lv_font_t *const FONT_SMALL = &lrt_font_14;

const lv_color_t COLOR_DELETE = LV_COLOR_MAKE(0x8B, 0x1E, 0x1E);
const int ROW_H = 52;

// File line and body
lv_obj_t *fileLine;
lv_obj_t *fileName;
lv_obj_t *fileDetail;
lv_obj_t *bodyError;
lv_obj_t *uploadGroup;
lv_obj_t *uploadText;
lv_obj_t *uploadBar;
lv_obj_t *useButton;

// The preview (specs/11-preview.md §2, §4, §5)
const int AREA_X = 8, AREA_Y = 80, AREA_W = 784, AREA_H = 204;
lv_obj_t *previewFrame;
PreviewCanvas previewCanvas;
bool canvasOk = false;
uint32_t drawnGeneration = 0;
lv_obj_t *fitButton;
lv_obj_t *previewGroup;
lv_obj_t *previewText;
lv_obj_t *previewBar;
lv_obj_t *previewInfo;  // in the preview area when there is no picture: why
lv_obj_t *retryButton;
lv_obj_t *infoLine;
lv_obj_t *warnLine;

// Picker
lv_obj_t *picker;
lv_obj_t *cardButtons[2];
lv_obj_t *pathLabel;
lv_obj_t *upButton;
lv_obj_t *listBox;
lv_obj_t *listStatus;
FileService::Card pickerCard = FileService::CARD_CONTROLLER;
char folders[2][FileService::PATH_LEN] = {"/sd", "/"};  // last folder used, per card
uint32_t shownGeneration = 0;
bool pickerWaiting = false;  // a listing was asked for and has not been shown

// Dialogs
lv_obj_t *deleteDialog;
lv_obj_t *deleteText;
char deletePath[FileService::PATH_LEN];
lv_obj_t *replaceDialog;
lv_obj_t *replaceText;

const char *cardName(FileService::Card card) { return card == FileService::CARD_CONTROLLER ? "Controller SD" : "Display SD"; }

// "/sd/jobs/x.gcode" -> "/jobs", for showing; the display SD's paths as they are
void shownFolder(char *out, int outLen, FileService::Card card, const char *path, bool isFile) {
    char folder[FileService::PATH_LEN];
    if (isFile) {
        FileService::parent(folder, sizeof(folder), card, path);
    } else {
        strncpy(folder, path, sizeof(folder) - 1);
        folder[sizeof(folder) - 1] = '\0';
    }
    const char *shown = folder;
    if (card == FileService::CARD_CONTROLLER && strncmp(folder, "/sd", 3) == 0) shown = folder + 3;
    snprintf(out, outLen, "%s", shown[0] ? shown : "/");
}

// --- Picker ------------------------------------------------------------------------

void listFolder(FileService::Card card, const char *folder) {
    pickerCard = card;
    strncpy(folders[card], folder, FileService::PATH_LEN - 1);
    FileService::list(card, folder);
    pickerWaiting = true;
    lv_obj_clean(listBox);
    listStatus = label(listBox, 0, 0, FONT_MID, UITheme::TEXT_MEDIUM, "Loading...");
}

void showCardButtons() {
    for (int i = 0; i < 2; i++) {
        setButtonColor(cardButtons[i], i == pickerCard ? UITheme::ACCENT_PRIMARY : UITheme::BG_BUTTON);
    }
}

void onRowClicked(lv_event_t *e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= FileService::count()) return;
    const FileService::Entry &entry = FileService::entry(index);
    if (entry.dir) {
        char path[FileService::PATH_LEN];
        FileService::join(path, sizeof(path), FileService::listFolder(), entry.name);
        listFolder(pickerCard, path);
        return;
    }
    JobService::choose(pickerCard, FileService::listFolder(), entry);
    lv_obj_add_flag(picker, LV_OBJ_FLAG_HIDDEN);
}

void onTrashClicked(lv_event_t *e) {
    if (MachineService::jobActive()) {
        Frame::notice("Not while a job runs.");
        return;
    }
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= FileService::count()) return;
    const FileService::Entry &entry = FileService::entry(index);
    FileService::join(deletePath, sizeof(deletePath), FileService::listFolder(), entry.name);
    char text[200];
    snprintf(text, sizeof(text), "Delete %s from the %s?", entry.name,
             pickerCard == FileService::CARD_CONTROLLER ? "controller's SD" : "display's SD");
    setText(deleteText, text);
    showDialog(deleteDialog, true);
}

void onDeleteConfirmed(lv_event_t *) {
    showDialog(deleteDialog, false);
    JobService::forget(pickerCard, deletePath);
    if (FileService::remove(pickerCard, deletePath)) {
        pickerWaiting = true;
    } else {
        Frame::notice(FileService::listError());
    }
}

void onDeleteCancelled(lv_event_t *) { showDialog(deleteDialog, false); }

lv_obj_t *makeRow(int index, const FileService::Entry &entry, bool allowDelete) {
    lv_obj_t *row = lv_obj_create(listBox);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    bool trash = !entry.dir && allowDelete;
    int width = 770 - (trash ? 72 : 0);
    char text[FileService::NAME_LEN + 8];
    snprintf(text, sizeof(text), "%s%s", entry.dir ? LV_SYMBOL_DIRECTORY "  " : "", entry.name);
    lv_obj_t *b = button(row, 0, 0, width, ROW_H, "", FONT_MID, onRowClicked, (void *)(intptr_t)index);
    lv_obj_t *name = lv_obj_get_child(b, 0);
    setText(name, text);  // through AppText: names may not be valid UTF-8
    lv_obj_set_width(name, width - 140);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
    if (!entry.dir) {
        char size[16];
        formatSize(size, sizeof(size), entry.size);
        lv_obj_t *sizeLabel = lv_label_create(b);
        lv_label_set_text(sizeLabel, size);
        lv_obj_set_style_text_font(sizeLabel, FONT_SMALL, 0);
        lv_obj_set_style_text_color(sizeLabel, UITheme::FILE_SIZE_TEXT, 0);
        lv_obj_align(sizeLabel, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    if (trash) {
        lv_obj_t *t = button(row, width + 8, 0, 64, ROW_H, LV_SYMBOL_TRASH, FONT_MID, onTrashClicked,
                             (void *)(intptr_t)index);
        setButtonColor(t, COLOR_DELETE);
    }
    return row;
}

void showListing() {
    shownGeneration = FileService::listGeneration();
    pickerWaiting = false;
    lv_obj_clean(listBox);
    listStatus = nullptr;
    if (FileService::listState() == FileService::LIST_FAILED) {
        listStatus = label(listBox, 0, 0, FONT_MID, UITheme::STATE_ALARM, FileService::listError());
        return;
    }
    if (FileService::count() == 0) {
        listStatus = label(listBox, 0, 0, FONT_MID, UITheme::TEXT_MEDIUM, "No G-code files or folders here.");
        return;
    }
    bool allowDelete = !MachineService::jobActive();
    for (int i = 0; i < FileService::count(); i++) makeRow(i, FileService::entry(i), allowDelete);
    if (FileService::truncated()) {
        label(listBox, 0, 0, FONT_SMALL, UITheme::TEXT_MEDIUM, "More files than can be shown; the rest are left out.");
    }
}

void onFileLineClicked(lv_event_t *) {
    lv_obj_clear_flag(picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(picker);
    // Opens on the card and folder last used
    showCardButtons();
    listFolder(pickerCard, folders[pickerCard]);
}

void onCardClicked(lv_event_t *e) {
    FileService::Card card = (FileService::Card)(intptr_t)lv_event_get_user_data(e);
    pickerCard = card;
    showCardButtons();
    listFolder(card, folders[card]);
}

void onUpClicked(lv_event_t *) {
    char up[FileService::PATH_LEN];
    FileService::parent(up, sizeof(up), pickerCard, folders[pickerCard]);
    listFolder(pickerCard, up);
}

void onCloseClicked(lv_event_t *) { lv_obj_add_flag(picker, LV_OBJ_FLAG_HIDDEN); }

void updatePicker() {
    if (lv_obj_has_flag(picker, LV_OBJ_FLAG_HIDDEN)) return;
    char shown[FileService::PATH_LEN];
    shownFolder(shown, sizeof(shown), pickerCard, folders[pickerCard], false);
    setText(pathLabel, shown);
    setEnabled(upButton, strcmp(folders[pickerCard], FileService::root(pickerCard)) != 0);
    if (pickerWaiting && FileService::listCard() == pickerCard &&
        FileService::listGeneration() != shownGeneration &&
        (FileService::listState() == FileService::LIST_READY || FileService::listState() == FileService::LIST_FAILED)) {
        showListing();
    }
}

// --- Use this job ---------------------------------------------------------------------

void onUseClicked(lv_event_t *) {
    if (MachineService::jobActive()) {
        Frame::notice("Not while a job runs.");
        return;
    }
    JobService::use();
}

void onCancelUpload(lv_event_t *) { JobService::cancelUse(); }
void onCancelPreview(lv_event_t *) { PreviewService::cancel(); }
void onRetryPreview(lv_event_t *) { PreviewService::retry(); }
void onReplace(lv_event_t *) { JobService::replace(true); }
void onKeep(lv_event_t *) { JobService::replace(false); }

// --- The preview's download and analysis ---------------------------------------------------

// A length for the info line: whole mm when it is one, else a tenth; inches to a hundredth
void formatLength(char *out, size_t outLen, double mm, bool inch) {
    if (inch) {
        snprintf(out, outLen, "%.2f", mm / 25.4);
    } else if (fabs(mm - round(mm)) < 0.05) {
        snprintf(out, outLen, "%.0f", mm);
    } else {
        snprintf(out, outLen, "%.1f", mm);
    }
    if (strcmp(out, "-0") == 0) snprintf(out, outLen, "0");
}

// "~2h10", "~25 min", "~1 min"; "+" when the estimate is short of some moves' time
void formatEstimate(char *out, size_t outLen, double seconds, bool incomplete) {
    long minutes = lround(seconds / 60.0);
    if (minutes < 1) minutes = 1;
    if (minutes >= 60) {
        snprintf(out, outLen, "~%ldh%02ld%s", minutes / 60, minutes % 60, incomplete ? "+" : "");
    } else {
        snprintf(out, outLen, "~%ld min%s", minutes, incomplete ? "+" : "");
    }
}

// "460 x 824 mm   Z -19..15   T2   90 ops   ~2h10"
void formatInfo(char *out, size_t outLen, const PreviewService::Summary &r) {
    char a[16], b[16];
    size_t n = 0;
    auto add = [&](const char *text) {
        int w = snprintf(out + n, outLen - n, "%s%s", n ? "   " : "", text);
        if (w > 0) n = n + (size_t)w < outLen ? n + (size_t)w : outLen - 1;
    };
    out[0] = '\0';
    char part[48];
    if (r.hasJob) {
        formatLength(a, sizeof(a), r.jobMaxX - r.jobMinX, r.inch);
        formatLength(b, sizeof(b), r.jobMaxY - r.jobMinY, r.inch);
        snprintf(part, sizeof(part), "%s x %s %s", a, b, r.inch ? "in" : "mm");
        add(part);
    }
    if (r.hasZ) {
        formatLength(a, sizeof(a), r.zMin, r.inch);
        formatLength(b, sizeof(b), r.zMax, r.inch);
        snprintf(part, sizeof(part), "Z %s..%s", a, b);
        add(part);
    }
    if (r.tools[0]) add(r.tools);
    if (r.ops >= 0) {
        snprintf(part, sizeof(part), "%d op%s", r.ops, r.ops == 1 ? "" : "s");
        add(part);
    }
    if (r.moves) {
        formatEstimate(part, sizeof(part), r.estimateSeconds, r.estimateIncomplete);
        add(part);
    }
}

// The preview's own warnings (P3, D82)
void formatWarnings(char *out, size_t outLen, const PreviewService::Summary &r) {
    out[0] = '\0';
    size_t n = 0;
    auto add = [&](const char *text) {
        int w = snprintf(out + n, outLen - n, "%s%s", n ? ";  " : "", text);
        if (w > 0) n = n + (size_t)w < outLen ? n + (size_t)w : outLen - 1;
    };
    char part[48];
    if (r.shownPercent < 100) {
        snprintf(part, sizeof(part), "Preview shows the first %u%% of the job", (unsigned)r.shownPercent);
        add(part);
    }
    if (r.unitsMixed) add("Mixed units");
    if (r.unitsAssumed) add("Units assumed: mm");
    if (r.skipped) add("Some moves had an unknown position and aren't drawn");
}

void show(lv_obj_t *o, bool visible) {
    if (visible) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

void onFitClicked(lv_event_t *) {
    previewCanvas.fit();
    previewCanvas.draw();
}

// The preview area and the info line. `chosen`: a job is chosen; `uploading`: the upload's bar
// has the area
void updatePreview(bool chosen, bool uploading) {
    PreviewService::State s = chosen ? PreviewService::state() : PreviewService::NONE;
    bool running = !uploading && (s == PreviewService::FETCHING || s == PreviewService::ANALYSING);
    if (running) {
        setText(previewText, s == PreviewService::FETCHING ? "Downloading from the controller for the preview..."
                                                           : "Analysing the job for the preview...");
        lv_bar_set_value(previewBar, (int32_t)(PreviewService::progress() * 1000), LV_ANIM_OFF);
    }
    show(previewGroup, running);

    const PreviewService::Summary &r = PreviewService::summary();
    bool ready = s == PreviewService::READY && PreviewService::segments();
    bool picture = ready && r.hasJob && canvasOk;
    if (picture && PreviewService::readyGeneration() != drawnGeneration) {
        drawnGeneration = PreviewService::readyGeneration();
        previewCanvas.fit();
        previewCanvas.draw();
        Serial.printf("JobScreen: preview drawn in %lu ms\n", (unsigned long)previewCanvas.lastDrawMs());
    }
    if (canvasOk) show(previewCanvas.object(), picture && !uploading);
    show(fitButton, picture && !uploading);

    char text[200] = "";
    bool problem = false;
    bool retry = false;
    switch (s) {
        case PreviewService::NONE:
            if (!chosen) snprintf(text, sizeof(text), "Choose a job to see its preview.");
            break;
        case PreviewService::READY:
            if (!r.hasJob) snprintf(text, sizeof(text), "No XY moves to draw.");
            else if (!canvasOk) snprintf(text, sizeof(text), "Not enough memory to draw the preview.");
            break;
        case PreviewService::NO_CARD:
            snprintf(text, sizeof(text), "Insert a display SD card to preview this job.");
            break;
        case PreviewService::WAITING_FOR_JOB:
            snprintf(text, sizeof(text), "Downloaded for the preview when the job ends.");
            break;
        case PreviewService::FAILED:
            snprintf(text, sizeof(text), "No preview: %s", PreviewService::message());
            problem = retry = true;
            break;
        case PreviewService::CANCELLED:
            snprintf(text, sizeof(text), "Preview cancelled.");
            retry = true;
            break;
        default:
            break;
    }
    setText(previewInfo, text);
    lv_obj_set_style_text_color(previewInfo, problem ? UITheme::STATE_ALARM : UITheme::TEXT_MEDIUM, 0);
    show(previewInfo, !running && !uploading && text[0]);
    show(retryButton, retry && !uploading);

    // The info line beside Use, with the preview's warnings under it
    char info[160] = "", warn[200] = "";
    if (s == PreviewService::READY) {
        formatInfo(info, sizeof(info), r);
        formatWarnings(warn, sizeof(warn), r);
    }
    setText(infoLine, info);
    setText(warnLine, warn);
}

// --- Building -----------------------------------------------------------------------------

void createFileLine(lv_obj_t *parent) {
    fileLine = button(parent, 8, 8, 784, 64, "", FONT_BIG, onFileLineClicked);
    lv_obj_t *icon = lv_obj_get_child(fileLine, 0);
    lv_label_set_text(icon, LV_SYMBOL_FILE);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);
    fileName = lv_label_create(fileLine);
    lv_obj_set_style_text_font(fileName, FONT_BIG, 0);
    lv_obj_set_width(fileName, 560);
    lv_label_set_long_mode(fileName, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(fileName, 44, 6);
    fileDetail = lv_label_create(fileLine);
    lv_obj_set_style_text_font(fileDetail, FONT_SMALL, 0);
    lv_obj_set_style_text_color(fileDetail, UITheme::TEXT_MEDIUM, 0);
    lv_obj_set_width(fileDetail, 560);
    lv_label_set_long_mode(fileDetail, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(fileDetail, 44, 34);
    lv_obj_t *change = lv_label_create(fileLine);
    lv_label_set_text(change, LV_SYMBOL_DIRECTORY "  Change");
    lv_obj_set_style_text_font(change, FONT_MID, 0);
    lv_obj_align(change, LV_ALIGN_RIGHT_MID, -16, 0);
}

void createBody(lv_obj_t *parent) {
    // The preview area: a frame, the canvas over it when there is a picture
    previewFrame = lv_obj_create(parent);
    lv_obj_set_pos(previewFrame, AREA_X - 1, AREA_Y - 1);
    lv_obj_set_size(previewFrame, AREA_W + 2, AREA_H + 2);
    lv_obj_set_style_bg_color(previewFrame, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(previewFrame, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(previewFrame, 1, 0);
    lv_obj_set_style_border_color(previewFrame, UITheme::BG_BUTTON, 0);
    lv_obj_set_style_radius(previewFrame, 0, 0);
    lv_obj_set_style_pad_all(previewFrame, 0, 0);
    lv_obj_clear_flag(previewFrame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(previewFrame, LV_OBJ_FLAG_CLICKABLE);
    canvasOk = previewCanvas.create(parent, AREA_X, AREA_Y, AREA_W, AREA_H);
    if (canvasOk) lv_obj_add_flag(previewCanvas.object(), LV_OBJ_FLAG_HIDDEN);
    fitButton = button(parent, AREA_X + AREA_W - 76, AREA_Y + 4, 72, 40, "Fit", FONT_MID, onFitClicked);
    lv_obj_set_style_bg_opa(fitButton, LV_OPA_80, 0);
    lv_obj_add_flag(fitButton, LV_OBJ_FLAG_HIDDEN);

    previewInfo = label(parent, AREA_X + 16, AREA_Y + 64, FONT_MID, UITheme::TEXT_MEDIUM, "");
    lv_obj_set_width(previewInfo, AREA_W - 32);
    lv_obj_set_style_text_align(previewInfo, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(previewInfo, LV_LABEL_LONG_WRAP);

    // The info line and warnings, left of Use this job
    infoLine = label(parent, 8, 300, FONT_MID, UITheme::TEXT_LIGHT, "");
    warnLine = label(parent, 8, 328, FONT_SMALL, UITheme::UI_WARNING, "");
    bodyError = label(parent, 8, 328, FONT_SMALL, UITheme::STATE_ALARM, "");
    for (lv_obj_t *l : {infoLine, warnLine, bodyError}) {
        lv_obj_set_width(l, 492);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }

    // The upload, in the preview area while it runs
    uploadGroup = lv_obj_create(parent);
    lv_obj_set_pos(uploadGroup, AREA_X, AREA_Y + 30);
    lv_obj_set_size(uploadGroup, AREA_W, 150);
    lv_obj_set_style_bg_opa(uploadGroup, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(uploadGroup, 0, 0);
    lv_obj_set_style_pad_all(uploadGroup, 0, 0);
    lv_obj_clear_flag(uploadGroup, LV_OBJ_FLAG_SCROLLABLE);
    uploadText = label(uploadGroup, 0, 0, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(uploadText, AREA_W);
    lv_obj_set_style_text_align(uploadText, LV_TEXT_ALIGN_CENTER, 0);
    uploadBar = lv_bar_create(uploadGroup);
    lv_obj_set_size(uploadBar, 600, 20);
    lv_obj_set_pos(uploadBar, 92, 40);
    lv_bar_set_range(uploadBar, 0, 1000);
    lv_obj_set_style_bg_color(uploadBar, UITheme::BG_BUTTON, 0);
    lv_obj_set_style_bg_color(uploadBar, UITheme::ACCENT_PRIMARY, LV_PART_INDICATOR);
    button(uploadGroup, 312, 84, 160, 56, "Cancel", FONT_MID, onCancelUpload);
    lv_obj_add_flag(uploadGroup, LV_OBJ_FLAG_HIDDEN);

    retryButton = button(parent, 312, AREA_Y + 120, 160, 48, LV_SYMBOL_REFRESH "  Retry", FONT_MID, onRetryPreview);
    lv_obj_add_flag(retryButton, LV_OBJ_FLAG_HIDDEN);

    // The download and analysis, in the preview area while they run
    previewGroup = lv_obj_create(parent);
    lv_obj_set_pos(previewGroup, AREA_X, AREA_Y + 40);
    lv_obj_set_size(previewGroup, AREA_W, 116);
    lv_obj_set_style_bg_opa(previewGroup, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(previewGroup, 0, 0);
    lv_obj_set_style_pad_all(previewGroup, 0, 0);
    lv_obj_clear_flag(previewGroup, LV_OBJ_FLAG_SCROLLABLE);
    previewText = label(previewGroup, 0, 0, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(previewText, AREA_W);
    lv_obj_set_style_text_align(previewText, LV_TEXT_ALIGN_CENTER, 0);
    previewBar = lv_bar_create(previewGroup);
    lv_obj_set_size(previewBar, 600, 16);
    lv_obj_set_pos(previewBar, 92, 32);
    lv_bar_set_range(previewBar, 0, 1000);
    lv_obj_set_style_bg_color(previewBar, UITheme::BG_BUTTON, 0);
    lv_obj_set_style_bg_color(previewBar, UITheme::ACCENT_PRIMARY, LV_PART_INDICATOR);
    button(previewGroup, 312, 60, 160, 48, "Cancel", FONT_MID, onCancelPreview);
    lv_obj_add_flag(previewGroup, LV_OBJ_FLAG_HIDDEN);

    useButton = button(parent, 512, 296, 280, 56, "Use this job  " LV_SYMBOL_RIGHT, FONT_BIG, onUseClicked);
    setButtonColor(useButton, UITheme::BTN_PLAY);
}

void createPicker(lv_obj_t *parent) {
    picker = lv_obj_create(parent);
    lv_obj_set_pos(picker, 0, 0);
    lv_obj_set_size(picker, 800, Frame::CONTENT_HEIGHT);
    lv_obj_set_style_bg_color(picker, UITheme::BG_DARKER, 0);
    lv_obj_set_style_bg_opa(picker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(picker, 0, 0);
    lv_obj_set_style_radius(picker, 0, 0);
    lv_obj_set_style_pad_all(picker, 0, 0);
    lv_obj_clear_flag(picker, LV_OBJ_FLAG_SCROLLABLE);

    cardButtons[0] = button(picker, 8, 8, 176, 48, "Controller SD", FONT_MID, onCardClicked,
                            (void *)(intptr_t)FileService::CARD_CONTROLLER);
    cardButtons[1] = button(picker, 192, 8, 160, 48, "Display SD", FONT_MID, onCardClicked,
                            (void *)(intptr_t)FileService::CARD_DISPLAY);
    pathLabel = label(picker, 368, 20, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(pathLabel, 240);
    lv_label_set_long_mode(pathLabel, LV_LABEL_LONG_DOT);
    upButton = button(picker, 616, 8, 84, 48, LV_SYMBOL_UP "  Up", FONT_MID, onUpClicked);
    button(picker, 708, 8, 84, 48, LV_SYMBOL_CLOSE, FONT_MID, onCloseClicked);

    listBox = lv_obj_create(picker);
    lv_obj_set_pos(listBox, 8, 64);
    lv_obj_set_size(listBox, 784, Frame::CONTENT_HEIGHT - 72);
    lv_obj_set_style_bg_opa(listBox, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(listBox, 0, 0);
    lv_obj_set_style_pad_all(listBox, 0, 0);
    lv_obj_set_style_pad_row(listBox, 4, 0);
    lv_obj_set_flex_flow(listBox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(listBox, LV_DIR_VER);

    lv_obj_add_flag(picker, LV_OBJ_FLAG_HIDDEN);
}

void createDialogs(lv_obj_t *parent) {
    deleteDialog = dialog(parent, 480, 196);
    deleteText = label(deleteDialog, 16, 16, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(deleteText, 448);
    lv_label_set_long_mode(deleteText, LV_LABEL_LONG_WRAP);
    lv_obj_t *del = button(deleteDialog, 16, 124, 216, 56, LV_SYMBOL_TRASH "  Delete", FONT_BIG, onDeleteConfirmed);
    setButtonColor(del, COLOR_DELETE);
    button(deleteDialog, 248, 124, 216, 56, "Cancel", FONT_BIG, onDeleteCancelled);

    replaceDialog = dialog(parent, 520, 216);
    replaceText = label(replaceDialog, 16, 16, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(replaceText, 488);
    lv_label_set_long_mode(replaceText, LV_LABEL_LONG_WRAP);
    button(replaceDialog, 16, 144, 236, 56, "Replace", FONT_BIG, onReplace);
    button(replaceDialog, 268, 144, 236, 56, "Cancel", FONT_BIG, onKeep);
}

}  // namespace

void JobScreen::create(lv_obj_t *parent) {
    createFileLine(parent);
    createBody(parent);
    createPicker(parent);
    createDialogs(parent);
}

void JobScreen::update() {
    const JobService::Choice &c = JobService::choice();
    char text[FileService::PATH_LEN + 64];
    char folder[FileService::PATH_LEN];
    char size[16];

    // The file line
    if (c.set) {
        shownFolder(folder, sizeof(folder), c.card, c.path, true);
        formatSize(size, sizeof(size), c.size);
        setText(fileName, c.name);
        snprintf(text, sizeof(text), "%s     %s  %s", size, cardName(c.card), folder);
        setText(fileDetail, text);
    } else {
        setText(fileName, "No job chosen");
        setText(fileDetail, "Tap to choose a file.");
    }

    // Use this job, and the upload it may start
    JobService::Prep prep = JobService::prep();
    bool uploading = prep == JobService::PREP_UPLOADING;
    if (uploading) {
        uint32_t total = FileService::uploadTotal();
        uint32_t sent = FileService::uploadSent();
        const char *what = FileService::uploadState() == FileService::UPLOAD_CHECKING    ? "Checking the controller..."
                           : FileService::uploadState() == FileService::UPLOAD_VERIFYING ? "Checking the upload..."
                                                                                         : "Uploading to the controller...";
        setText(uploadText, what);
        lv_bar_set_value(uploadBar, total ? (int32_t)((uint64_t)sent * 1000 / total) : 0, LV_ANIM_OFF);
        lv_obj_clear_flag(uploadGroup, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(uploadGroup, LV_OBJ_FLAG_HIDDEN);
    }
    updatePreview(c.set, uploading);

    bool confirming = prep == JobService::PREP_CONFIRM_REPLACE;
    if (confirming && lv_obj_has_flag(lv_obj_get_parent(replaceDialog), LV_OBJ_FLAG_HIDDEN)) {
        snprintf(text, sizeof(text), "%s is already on the controller, in %s. Replace it?", c.name,
                 FileService::UPLOAD_FOLDER + 3);
        setText(replaceText, text);
        showDialog(replaceDialog, true);
    } else if (!confirming) {
        showDialog(replaceDialog, false);
    }

    bool prepFailed = prep == JobService::PREP_FAILED;
    setText(bodyError, prepFailed ? JobService::prepError() : "");
    show(warnLine, !prepFailed);

    if (prep == JobService::PREP_READY) {
        JobService::prepTaken();
        Frame::show(Frame::SCREEN_RUN);
        return;
    }

    bool canUse = c.set && MachineService::online() && !MachineService::jobActive() && !uploading && !confirming;
    setEnabled(useButton, canUse);
    setEnabled(fileLine, !uploading && !confirming);

    updatePicker();
}
