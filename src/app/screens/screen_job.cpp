#include "app/screens/screens.h"
#include "app/fonts.h"
#include "app/frame.h"
#include "app/widgets.h"
#include "services/file_service.h"
#include "services/job_service.h"
#include "services/machine_service.h"
#include "services/preview_service.h"
#include "ui/ui_theme.h"
#include <Arduino.h>
#include <string.h>

// The Job screen (specs/03-screens.md section 3). Content area 800 x 360:
//
//   y   8..72   the chosen file; tapping it opens the file picker
//   y  80..288  preview (M5); until part 5b the file in large type with the analysis's
//               result, or the download and analysis with their bar, or the upload
//   y 296..352  Use this job
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
lv_obj_t *bodyName;
lv_obj_t *bodyDetail;
lv_obj_t *bodyError;
lv_obj_t *uploadGroup;
lv_obj_t *uploadText;
lv_obj_t *uploadBar;
lv_obj_t *useButton;

// The preview's download and analysis (specs/11-preview.md §2)
lv_obj_t *previewGroup;
lv_obj_t *previewText;
lv_obj_t *previewBar;
lv_obj_t *previewInfo;
lv_obj_t *retryButton;

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

// "14,189 lines" with thousands separators
void formatCount(char *out, size_t outLen, uint32_t n) {
    if (n >= 1000000) {
        snprintf(out, outLen, "%lu,%03lu,%03lu", (unsigned long)(n / 1000000), (unsigned long)(n / 1000 % 1000),
                 (unsigned long)(n % 1000));
    } else if (n >= 1000) {
        snprintf(out, outLen, "%lu,%03lu", (unsigned long)(n / 1000), (unsigned long)(n % 1000));
    } else {
        snprintf(out, outLen, "%lu", (unsigned long)n);
    }
}

// Until the preview is drawn (part 5b): the analysis's result as a line under the file
void updatePreview(bool show) {
    PreviewService::State s = show ? PreviewService::state() : PreviewService::NONE;
    bool running = s == PreviewService::FETCHING || s == PreviewService::ANALYSING;
    if (running) {
        setText(previewText, s == PreviewService::FETCHING ? "Downloading from the controller for the preview..."
                                                           : "Analysing the job for the preview...");
        lv_bar_set_value(previewBar, (int32_t)(PreviewService::progress() * 1000), LV_ANIM_OFF);
        lv_obj_clear_flag(previewGroup, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(previewGroup, LV_OBJ_FLAG_HIDDEN);
    }

    char text[200] = "";
    bool problem = false;
    bool retry = false;
    switch (s) {
        case PreviewService::READY: {
            const PreviewService::Summary &r = PreviewService::summary();
            char lines[16];
            formatCount(lines, sizeof(lines), r.lines);
            if (r.hasExtents) {
                snprintf(text, sizeof(text), "%s lines   %.0f x %.0f %s   analysed in %.1f s", lines, r.maxX - r.minX,
                         r.maxY - r.minY, r.inch ? "in" : "mm", r.analyseMs / 1000.0);
            } else {
                snprintf(text, sizeof(text), "%s lines, no XY moves   analysed in %.1f s", lines, r.analyseMs / 1000.0);
            }
            break;
        }
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
    if (running) {
        lv_obj_add_flag(previewInfo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(previewInfo, LV_OBJ_FLAG_HIDDEN);
    }
    if (retry) {
        lv_obj_clear_flag(retryButton, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(retryButton, LV_OBJ_FLAG_HIDDEN);
    }
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
    bodyName = label(parent, 8, 100, FONT_LARGE, UITheme::TEXT_LIGHT, "");
    bodyDetail = label(parent, 8, 136, FONT_MID, UITheme::TEXT_MEDIUM, "");
    previewInfo = label(parent, 8, 172, FONT_MID, UITheme::TEXT_MEDIUM, "");
    bodyError = label(parent, 8, 262, FONT_MID, UITheme::STATE_ALARM, "");
    for (lv_obj_t *l : {bodyName, bodyDetail, previewInfo, bodyError}) {
        lv_obj_set_width(l, 784);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }

    // The upload, in place of the details while it runs
    uploadGroup = lv_obj_create(parent);
    lv_obj_set_pos(uploadGroup, 8, 120);
    lv_obj_set_size(uploadGroup, 784, 160);
    lv_obj_set_style_bg_opa(uploadGroup, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(uploadGroup, 0, 0);
    lv_obj_set_style_pad_all(uploadGroup, 0, 0);
    lv_obj_clear_flag(uploadGroup, LV_OBJ_FLAG_SCROLLABLE);
    uploadText = label(uploadGroup, 0, 0, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(uploadText, 784);
    lv_obj_set_style_text_align(uploadText, LV_TEXT_ALIGN_CENTER, 0);
    uploadBar = lv_bar_create(uploadGroup);
    lv_obj_set_size(uploadBar, 600, 20);
    lv_obj_set_pos(uploadBar, 92, 40);
    lv_bar_set_range(uploadBar, 0, 1000);
    lv_obj_set_style_bg_color(uploadBar, UITheme::BG_BUTTON, 0);
    lv_obj_set_style_bg_color(uploadBar, UITheme::ACCENT_PRIMARY, LV_PART_INDICATOR);
    button(uploadGroup, 312, 84, 160, 56, "Cancel", FONT_MID, onCancelUpload);
    lv_obj_add_flag(uploadGroup, LV_OBJ_FLAG_HIDDEN);

    retryButton = button(parent, 312, 204, 160, 48, LV_SYMBOL_REFRESH "  Retry", FONT_MID, onRetryPreview);
    lv_obj_add_flag(retryButton, LV_OBJ_FLAG_HIDDEN);

    // The download and analysis, under the file's name while they run
    previewGroup = lv_obj_create(parent);
    lv_obj_set_pos(previewGroup, 8, 172);
    lv_obj_set_size(previewGroup, 784, 116);
    lv_obj_set_style_bg_opa(previewGroup, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(previewGroup, 0, 0);
    lv_obj_set_style_pad_all(previewGroup, 0, 0);
    lv_obj_clear_flag(previewGroup, LV_OBJ_FLAG_SCROLLABLE);
    previewText = label(previewGroup, 0, 0, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(previewText, 784);
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

    // The file line, and the file in large type until the preview exists
    if (c.set) {
        shownFolder(folder, sizeof(folder), c.card, c.path, true);
        formatSize(size, sizeof(size), c.size);
        setText(fileName, c.name);
        snprintf(text, sizeof(text), "%s     %s  %s", size, cardName(c.card), folder);
        setText(fileDetail, text);
        setText(bodyName, c.name);
        snprintf(text, sizeof(text), "%s on the %s, %s", size,
                 c.card == FileService::CARD_CONTROLLER ? "controller's SD" : "display's SD", folder);
        setText(bodyDetail, text);
    } else {
        setText(fileName, "No job chosen");
        setText(fileDetail, "Tap to choose a file.");
        setText(bodyName, "");
        setText(bodyDetail, "");
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
        lv_obj_add_flag(bodyDetail, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(uploadGroup, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(bodyDetail, LV_OBJ_FLAG_HIDDEN);
    }
    updatePreview(c.set && !uploading);

    bool confirming = prep == JobService::PREP_CONFIRM_REPLACE;
    if (confirming && lv_obj_has_flag(lv_obj_get_parent(replaceDialog), LV_OBJ_FLAG_HIDDEN)) {
        snprintf(text, sizeof(text), "%s is already on the controller, in %s. Replace it?", c.name,
                 FileService::UPLOAD_FOLDER + 3);
        setText(replaceText, text);
        showDialog(replaceDialog, true);
    } else if (!confirming) {
        showDialog(replaceDialog, false);
    }

    setText(bodyError, prep == JobService::PREP_FAILED ? JobService::prepError() : "");

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
