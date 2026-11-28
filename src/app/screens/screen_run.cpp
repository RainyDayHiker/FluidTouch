#include "app/screens/screens.h"
#include "app/fonts.h"
#include "app/frame.h"
#include "app/preview_canvas.h"
#include "app/widgets.h"
#include "services/job_service.h"
#include "services/machine_service.h"
#include "services/preview_service.h"
#include "services/run_follow.h"
#include "services/run_preview.h"
#include "ui/ui_theme.h"
#include <Arduino.h>
#include <string.h>

// The Run screen (specs/03-screens.md section 4). Content area 800 x 360:
//
//   x   8..576  the preview (specs/11-preview.md §6, D89, D91): 568 x 344, a box over its
//               top half at a prompt; without a preview of this job, the file and the
//               current message in large type, and during a cut the offer to download it
//               (D72, D94)
//   x 592..792  the panel (D91): START; or op N of M, progress, times, feed override and PAUSE
//               (CONTINUE at a prompt; ABORT is the status bar's STOP during a job, D90);
//               or the result
//
// With no job chosen or running, one line and a button to the Job tab.

using namespace Widgets;

namespace {

const lv_font_t *const FONT_HUGE = &lrt_font_32;
const lv_font_t *const FONT_LARGE = &lrt_font_26;
const lv_font_t *const FONT_BIG = &lrt_font_20;
const lv_font_t *const FONT_MID = &lrt_font_18;
const lv_font_t *const FONT_SMALL = &lrt_font_14;

const int LEFT_X = 16, LEFT_W = 552;
const int LEFT_GROUP_W = 584;  // the left side, up to the divider
const int PANEL_X = 592, PANEL_W = 200;
const int OP_LEFT_W = 96;  // "~12:34 left"
const int PREVIEW_X = 8, PREVIEW_Y = 8, PREVIEW_W = 568, PREVIEW_H = 344;

lv_obj_t *noneGroup;
lv_obj_t *jobGroup;      // everything but noneGroup

// Left, with a preview of the running job: the canvas, Fit, the prompt box
PreviewCanvas canvas;
bool canvasOk = false;
lv_obj_t *previewGroup;
lv_obj_t *fitButton;
lv_obj_t *promptBox;
lv_obj_t *promptText;
uint32_t drawnReady = 0;   // PreviewService::readyGeneration() drawn
uint32_t drawnRun = 0;     // RunFollow::runGeneration() drawn
bool drawnOnce = false;

// Left, without one: the file and the message
lv_obj_t *textGroup;
lv_obj_t *leftName;
lv_obj_t *leftDetail;
lv_obj_t *leftHeading;   // "Waiting for you" at a prompt
lv_obj_t *leftMessage;
lv_obj_t *downloadButton;  // the offer (D72, D94)
lv_obj_t *downloadText;    // its progress, or why the last try failed
lv_obj_t *downloadDialog;
lv_obj_t *downloadDialogText;
lv_obj_t *useCopyButton;      // in the dialog when only the name vouches for a copy (D98)
lv_obj_t *dialogDownloadButton;
lv_obj_t *dialogNotNowButton;

// Panel groups, one shown at a time
lv_obj_t *startGroup;
lv_obj_t *runGroup;
lv_obj_t *endGroup;

// Run group
lv_obj_t *opLabel;       // op N of M
lv_obj_t *opLeftLabel;   // the operation's time left, right-aligned beside it (D100)
lv_obj_t *progressBar;
lv_obj_t *percentLabel;
lv_obj_t *elapsedValue;
lv_obj_t *leftCaption;
lv_obj_t *leftValue;
lv_obj_t *feedRateLabel;
lv_obj_t *overrideGroup;
lv_obj_t *feedValue;
lv_obj_t *feedButtons[3];
lv_obj_t *continueButton;
lv_obj_t *pauseButton;

// End group
lv_obj_t *endTitle;
lv_obj_t *endDetail;

lv_obj_t *group(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_pos(g, x, y);
    lv_obj_set_size(g, w, h);
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_radius(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    return g;
}

void show(lv_obj_t *obj, bool visible) {
    if (visible == !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (visible) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void divider(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *line = group(parent, x, y, w, h);
    lv_obj_set_style_bg_color(line, UITheme::BORDER_MEDIUM, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
}

// --- Events ----------------------------------------------------------------------

void onGoToJob(lv_event_t *) { Frame::show(Frame::SCREEN_JOB); }
void onFit(lv_event_t *) {
    canvas.fit();
    canvas.draw();
}
void onStart(lv_event_t *) {
    if (!MachineService::canZero()) {
        Frame::notice(MachineService::busyReason());
        return;
    }
    JobService::start();
}
void onPause(lv_event_t *) {
    if (JobService::runState() == JobService::RUN_PAUSED) {
        JobService::resume();
    } else {
        JobService::pause();
    }
}
void onContinue(lv_event_t *) { JobService::resume(); }
void onFeed(lv_event_t *e) {
    MachineService::feedOverride((MachineService::FeedOverride)(intptr_t)lv_event_get_user_data(e));
}
void onDownloadOffer(lv_event_t *) {
    // The confirm of 05 §2 (D72), with the size when the listing gave it
    char size[16] = "";
    if (RunPreview::size() >= 0) {
        char s[12];
        formatSize(s, sizeof(s), RunPreview::size());
        snprintf(size, sizeof(size), " (%s)", s);
    }
    char other[FileService::NAME_LEN + 48] = "";
    if (RunPreview::otherChosen()) snprintf(other, sizeof(other), "\n\nIt replaces %s as the chosen job.", RunPreview::otherName());
    char text[FileService::NAME_LEN * 2 + 360];
    bool copy = RunPreview::nameCopy() >= 0;
    if (copy) {
        // Only the name vouches for the copy: the operator knows whether it is the same file (D98)
        char s[12];
        formatSize(s, sizeof(s), RunPreview::nameCopy());
        snprintf(text, sizeof(text),
                 "The display has a copy of %s from before (%s). The controller didn't give the file's size "
                 "(%s), so it can't be checked that the copy is the same file.\n\nUse the copy, or download "
                 "the file now, while the machine is cutting?%s",
                 JobService::runName(), s, RunPreview::sizeError(), other);
    } else {
        snprintf(text, sizeof(text),
                 "Download %s%s now, while the machine is cutting?\n\nThe controller reads its SD card for the "
                 "download as well as for the job.%s",
                 JobService::runName(), size, other);
    }
    setText(downloadDialogText, text);
    // Three buttons with the copy, two without
    show(useCopyButton, copy);
    int w = copy ? 184 : 276;
    lv_obj_set_size(dialogDownloadButton, w, 56);
    lv_obj_set_size(dialogNotNowButton, w, 56);
    lv_obj_set_x(dialogDownloadButton, copy ? 16 + w + 8 : 16);
    lv_obj_set_x(dialogNotNowButton, copy ? 16 + 2 * (w + 8) : 16 + w + 16);
    showDialog(downloadDialog, true);
}
void onUseCopy(lv_event_t *) {
    showDialog(downloadDialog, false);
    RunPreview::useCopy();
}
void onDownloadConfirmed(lv_event_t *) {
    showDialog(downloadDialog, false);
    RunPreview::download();
}
void onDownloadNotNow(lv_event_t *) { showDialog(downloadDialog, false); }
void onAgain(lv_event_t *) { JobService::again(); }
void onBackToSetup(lv_event_t *) {
    JobService::dismiss();
    Frame::show(Frame::SCREEN_SETUP);
}

// --- Building ---------------------------------------------------------------------------

void createNone(lv_obj_t *parent) {
    noneGroup = group(parent, 0, 0, 800, Frame::CONTENT_HEIGHT);
    lv_obj_t *text = label(noneGroup, 0, 120, FONT_BIG, UITheme::TEXT_LIGHT, "No job. Choose one on the Job tab.");
    lv_obj_set_width(text, 800);
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    button(noneGroup, 300, 176, 200, 56, "Go to Job", FONT_BIG, onGoToJob);
}

void createPreview(lv_obj_t *parent) {
    previewGroup = group(parent, 0, 0, LEFT_GROUP_W, Frame::CONTENT_HEIGHT);
    canvasOk = canvas.create(previewGroup, PREVIEW_X, PREVIEW_Y, PREVIEW_W, PREVIEW_H, true);
    fitButton = button(previewGroup, PREVIEW_X + PREVIEW_W - 76, PREVIEW_Y + 4, 72, 40, "Fit", FONT_MID, onFit);
    lv_obj_set_style_bg_opa(fitButton, LV_OPA_80, 0);

    // At a prompt: the message over the preview's top half, the tool still in view below
    promptBox = group(previewGroup, PREVIEW_X, PREVIEW_Y, PREVIEW_W, PREVIEW_H / 2);
    lv_obj_set_style_bg_color(promptBox, UITheme::BG_DARKER, 0);
    lv_obj_set_style_bg_opa(promptBox, LV_OPA_90, 0);
    lv_obj_set_style_border_color(promptBox, UITheme::STATE_HOLD, 0);
    lv_obj_set_style_border_width(promptBox, 2, 0);
    label(promptBox, 12, 8, FONT_MID, UITheme::STATE_HOLD, "Waiting for you");
    promptText = label(promptBox, 12, 36, FONT_LARGE, UITheme::TEXT_LIGHT, "");
    lv_obj_set_size(promptText, PREVIEW_W - 24, PREVIEW_H / 2 - 44);
    lv_label_set_long_mode(promptText, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(promptBox, LV_OBJ_FLAG_HIDDEN);
}

void createLeft(lv_obj_t *parent) {
    textGroup = group(parent, 0, 0, LEFT_GROUP_W, Frame::CONTENT_HEIGHT);
    leftName = label(textGroup, LEFT_X, 12, FONT_BIG, UITheme::TEXT_LIGHT, "");
    leftDetail = label(textGroup, LEFT_X, 42, FONT_SMALL, UITheme::TEXT_MEDIUM, "");
    leftHeading = label(textGroup, LEFT_X, 92, FONT_MID, UITheme::STATE_HOLD, "Waiting for you");
    leftMessage = label(textGroup, LEFT_X, 124, FONT_LARGE, UITheme::TEXT_LIGHT, "");
    for (lv_obj_t *l : {leftName, leftDetail, leftMessage}) lv_obj_set_width(l, LEFT_W);
    lv_label_set_long_mode(leftName, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(leftDetail, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(leftMessage, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(leftMessage, 160);  // clear of the offer below
    // The offer, at the bottom (D94)
    downloadButton = button(textGroup, LEFT_X, 288, 240, 56, LV_SYMBOL_DOWNLOAD "  Download preview", FONT_BIG,
                            onDownloadOffer);
    downloadText = label(textGroup, LEFT_X + 256, 288, FONT_SMALL, UITheme::TEXT_MEDIUM, "");
    lv_obj_set_size(downloadText, LEFT_W - 256, 56);
    lv_label_set_long_mode(downloadText, LV_LABEL_LONG_WRAP);
    divider(parent, LEFT_GROUP_W, 8, 1, Frame::CONTENT_HEIGHT - 16);
}

void createDownloadDialog(lv_obj_t *parent) {
    downloadDialog = dialog(parent, 600, 300);
    downloadDialogText = label(downloadDialog, 16, 16, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_size(downloadDialogText, 568, 196);
    lv_label_set_long_mode(downloadDialogText, LV_LABEL_LONG_WRAP);
    useCopyButton = button(downloadDialog, 16, 228, 184, 56, LV_SYMBOL_OK "  Use the copy", FONT_BIG, onUseCopy);
    dialogDownloadButton =
        button(downloadDialog, 16, 228, 276, 56, LV_SYMBOL_DOWNLOAD "  Download", FONT_BIG, onDownloadConfirmed);
    dialogNotNowButton = button(downloadDialog, 308, 228, 276, 56, "Not now", FONT_BIG, onDownloadNotNow);
}

// The offer under the file and message (D72, D94): the button, and the download's progress
// or why it failed
void updateOffer() {
    RunPreview::State st = RunPreview::state();
    show(downloadButton, st == RunPreview::OFFER);
    char text[200] = "";
    if (st == RunPreview::LOADING) {
        PreviewService::State ps = PreviewService::state();
        snprintf(text, sizeof(text), "%s the preview: %d%%",
                 ps == PreviewService::FETCHING ? "Downloading" : "Analysing", (int)(PreviewService::progress() * 100));
    } else if (st == RunPreview::OFFER && RunPreview::failure()[0]) {
        snprintf(text, sizeof(text), "%s", RunPreview::failure());
    } else if (st == RunPreview::OFFER && RunPreview::sizeError()[0]) {
        // Why the copy from before wasn't used without asking (D98)
        snprintf(text, sizeof(text), "The controller didn't give the size (%s).%s", RunPreview::sizeError(),
                 RunPreview::nameCopy() >= 0 ? " A copy from before is on the display." : "");
    }
    // The button names what it leads to: a copy to use, or only the download
    setText(lv_obj_get_child(downloadButton, 0), RunPreview::nameCopy() >= 0 ? LV_SYMBOL_IMAGE "  Load preview"
                                                                              : LV_SYMBOL_DOWNLOAD "  Download preview");
    show(downloadText, text[0] != '\0');
    setText(downloadText, text);
    // Beside the button, or in its place while it is hidden
    int x = st == RunPreview::OFFER ? LEFT_X + 256 : LEFT_X;
    if (lv_obj_get_x(downloadText) != x) {
        lv_obj_set_x(downloadText, x);
        lv_obj_set_width(downloadText, LEFT_GROUP_W - x - 16);
    }
    if (st != RunPreview::OFFER) {
        lv_obj_t *layer = lv_obj_get_parent(downloadDialog);
        if (!lv_obj_has_flag(layer, LV_OBJ_FLAG_HIDDEN)) showDialog(downloadDialog, false);
    }
}

void createStart(lv_obj_t *parent) {
    startGroup = group(parent, PANEL_X, 0, PANEL_W, Frame::CONTENT_HEIGHT);
    lv_obj_t *start = button(startGroup, 0, 96, PANEL_W, 160, LV_SYMBOL_PLAY "  START", FONT_HUGE, onStart);
    setButtonColor(start, UITheme::BTN_PLAY);
}

// A label right-aligned so its right edge stays at `right`, whatever the text's width
lv_obj_t *rightLabel(lv_obj_t *parent, int right, int y, int w, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *l = label(parent, right - w, y, font, color, "");
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    return l;
}

void createRun(lv_obj_t *parent) {
    runGroup = group(parent, PANEL_X, 0, PANEL_W, Frame::CONTENT_HEIGHT);

    // op N of M, the op's time left right-aligned as the values below it (so neither moves the
    // other); the bar with the percent at its right
    opLabel = label(runGroup, 0, 8, FONT_MID, UITheme::TEXT_MEDIUM, "");
    lv_obj_set_width(opLabel, PANEL_W - OP_LEFT_W);
    lv_label_set_long_mode(opLabel, LV_LABEL_LONG_DOT);
    opLeftLabel = rightLabel(runGroup, PANEL_W, 8, OP_LEFT_W, FONT_MID, UITheme::TEXT_LIGHT);
    progressBar = lv_bar_create(runGroup);
    lv_obj_set_pos(progressBar, 0, 40);
    lv_obj_set_size(progressBar, PANEL_W - 60, 24);
    lv_bar_set_range(progressBar, 0, 1000);
    lv_obj_set_style_bg_color(progressBar, UITheme::BG_BUTTON, 0);
    lv_obj_set_style_bg_color(progressBar, UITheme::BG_PROGRESS, LV_PART_INDICATOR);
    percentLabel = rightLabel(runGroup, PANEL_W, 42, 120, FONT_MID, UITheme::TEXT_LIGHT);

    // The times: captions where they are, values right-aligned
    label(runGroup, 0, 78, FONT_MID, UITheme::TEXT_MEDIUM, "Elapsed");
    elapsedValue = rightLabel(runGroup, PANEL_W, 78, 110, FONT_MID, UITheme::TEXT_LIGHT);
    leftCaption = label(runGroup, 0, 104, FONT_MID, UITheme::TEXT_MEDIUM, "Left");
    leftValue = rightLabel(runGroup, PANEL_W, 104, 110, FONT_MID, UITheme::TEXT_LIGHT);
    // The feed, or two lines when the connection is lost
    feedRateLabel = label(runGroup, 0, 132, FONT_MID, UITheme::TEXT_MEDIUM, "");
    lv_obj_set_size(feedRateLabel, PANEL_W, 44);
    lv_label_set_long_mode(feedRateLabel, LV_LABEL_LONG_WRAP);
    divider(runGroup, 0, 180, PANEL_W, 1);

    // The feed override, 100 in the middle; CONTINUE takes its place at a prompt
    overrideGroup = group(runGroup, 0, 190, PANEL_W, 84);
    label(overrideGroup, 0, 0, FONT_MID, UITheme::TEXT_MEDIUM, "Feed override");
    feedValue = rightLabel(overrideGroup, PANEL_W, 0, 60, FONT_MID, UITheme::TEXT_LIGHT);
    const char *const feedTexts[3] = {"-10", "100", "+10"};
    const MachineService::FeedOverride feedSteps[3] = {MachineService::FEED_MINUS_10, MachineService::FEED_RESET,
                                                       MachineService::FEED_PLUS_10};
    for (int i = 0; i < 3; i++) {
        feedButtons[i] = button(overrideGroup, i * 69, 30, 62, 52, feedTexts[i], FONT_BIG, onFeed,
                                (void *)(intptr_t)feedSteps[i]);
    }
    continueButton = button(runGroup, 0, 190, PANEL_W, 84, LV_SYMBOL_PLAY "  CONTINUE", FONT_LARGE, onContinue);
    setButtonColor(continueButton, UITheme::BTN_PLAY);

    divider(runGroup, 0, 288, PANEL_W, 1);
    pauseButton = button(runGroup, 0, 296, PANEL_W, 56, LV_SYMBOL_PAUSE "  PAUSE", FONT_BIG, onPause);
    setButtonColor(pauseButton, UITheme::STATE_HOLD);
}

void createEnd(lv_obj_t *parent) {
    endGroup = group(parent, PANEL_X, 0, PANEL_W, Frame::CONTENT_HEIGHT);
    endTitle = label(endGroup, 0, 40, FONT_HUGE, UITheme::STATE_IDLE, "");
    endDetail = label(endGroup, 0, 92, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(endDetail, PANEL_W);
    lv_label_set_long_mode(endDetail, LV_LABEL_LONG_WRAP);
    button(endGroup, 0, 224, PANEL_W, 60, "Run again", FONT_BIG, onAgain);
    button(endGroup, 0, 292, PANEL_W, 60, "Back to Setup", FONT_BIG, onBackToSetup);
}

// --- Updating -----------------------------------------------------------------------------

void updateRunGroup(JobService::RunState state, bool online) {
    const FluidNCStatus &s = MachineService::status();
    char text[96];

    int ops = PreviewService::summary().ops, op = RunFollow::op();
    if (ops > 0 && op > 0) {
        snprintf(text, sizeof(text), "op %d of %d", op, ops);
    } else {
        text[0] = '\0';
    }
    setText(opLabel, text);
    // To the end of the op's last move; hidden between operations (D100)
    uint32_t opLeft = ops > 0 && op > 0 ? RunFollow::opLeftMs() : 0;
    if (opLeft) {
        text[0] = '~';
        formatDuration(text + 1, sizeof(text) - 1, opLeft);
        strncat(text, " left", sizeof(text) - strlen(text) - 1);
    } else {
        text[0] = '\0';
    }
    setText(opLeftLabel, text);

    float pct = JobService::percent();
    lv_bar_set_value(progressBar, (int32_t)(pct * 10), LV_ANIM_OFF);
    snprintf(text, sizeof(text), "%d%%", (int)pct);
    setText(percentLabel, state == JobService::RUN_STARTING ? "Starting..." : text);
    formatDuration(text, sizeof(text), JobService::elapsedMs());
    setText(elapsedValue, text);
    // From the plan while the tool is followed (11 §7, D91), else from the percent (D90)
    uint32_t leftMs = RunFollow::plansTime() ? RunFollow::leftMs() : JobService::leftMs();
    show(leftCaption, leftMs != 0);
    show(leftValue, leftMs != 0);
    if (leftMs) {
        text[0] = '~';
        formatDuration(text + 1, sizeof(text) - 1, leftMs);
        setText(leftValue, text);
    }
    if (online) {
        snprintf(text, sizeof(text), "feed %d mm/min", (int)s.feed_rate);
    } else {
        snprintf(text, sizeof(text), "No connection; the job continues");
    }
    setText(feedRateLabel, text);

    bool prompt = state == JobService::RUN_PROMPT;
    show(overrideGroup, !prompt);
    show(continueButton, prompt);
    setEnabled(continueButton, online);

    snprintf(text, sizeof(text), "%d%%", (int)s.feed_override);
    setText(feedValue, online ? text : "-");
    bool overrides = online && state != JobService::RUN_STARTING;
    for (lv_obj_t *b : feedButtons) setEnabled(b, overrides);

    bool paused = state == JobService::RUN_PAUSED;
    lv_obj_t *pauseLabel = lv_obj_get_child(pauseButton, 0);
    setText(pauseLabel, paused ? LV_SYMBOL_PLAY "  RESUME" : LV_SYMBOL_PAUSE "  PAUSE");
    setButtonColor(pauseButton, paused ? UITheme::BTN_PLAY : UITheme::STATE_HOLD);
    setEnabled(pauseButton, online && (state == JobService::RUN_RUNNING || paused));
}

void updateEndGroup(JobService::RunState state) {
    char text[200];
    char total[16];
    bool finished = state == JobService::RUN_FINISHED;
    setText(endTitle, finished ? "Finished" : "Stopped");
    setTextColor(endTitle, finished ? UITheme::STATE_IDLE : UITheme::STATE_ALARM);
    formatDuration(total, sizeof(total), JobService::elapsedMs());
    if (finished) {
        snprintf(text, sizeof(text), "Total time %s", total);
    } else {
        snprintf(text, sizeof(text), "%s\nAfter %s", JobService::endText(), total);
    }
    setText(endDetail, text);
}

// The preview of the running job, when there is one: true when it is shown
bool updatePreview(JobService::RunState state, bool online) {
    bool shown = canvasOk && RunFollow::following();
    show(previewGroup, shown);
    show(textGroup, !shown);
    if (!shown) {
        drawnOnce = false;
        return false;
    }
    if (!drawnOnce || PreviewService::readyGeneration() != drawnReady || RunFollow::runGeneration() != drawnRun) {
        drawnOnce = true;
        drawnReady = PreviewService::readyGeneration();
        drawnRun = RunFollow::runGeneration();
        canvas.restart(RunFollow::doneRecord());
    } else {
        canvas.setDone(RunFollow::doneRecord());
    }
    const FluidNCStatus &s = MachineService::status();
    canvas.setMarker(online && JobService::active(), s.wpos_x, s.wpos_y);

    bool prompt = state == JobService::RUN_PROMPT;
    show(promptBox, prompt);
    show(fitButton, !prompt);
    if (prompt) setText(promptText, JobService::promptText());
    return true;
}

}  // namespace

void RunScreen::create(lv_obj_t *parent) {
    createNone(parent);
    jobGroup = group(parent, 0, 0, 800, Frame::CONTENT_HEIGHT);
    createPreview(jobGroup);
    createLeft(jobGroup);
    createStart(jobGroup);
    createRun(jobGroup);
    createEnd(jobGroup);
    createDownloadDialog(parent);
    update();  // one panel shown from the start
}

void RunScreen::update() {
    JobService::RunState state = JobService::runState();
    bool online = MachineService::online();
    bool none = state == JobService::RUN_NONE;
    show(noneGroup, none);
    show(jobGroup, !none);
    if (none) return;

    bool ready = state == JobService::RUN_READY;
    bool ended = state == JobService::RUN_FINISHED || state == JobService::RUN_ABORTED;
    show(startGroup, ready);
    show(runGroup, JobService::active());
    show(endGroup, ended);

    // Left: the file, then the prompt or the current message
    char text[FileService::PATH_LEN + 32];
    char size[16];
    setText(leftName, JobService::runName());
    if (JobService::runSize() >= 0) {
        formatSize(size, sizeof(size), JobService::runSize());
        snprintf(text, sizeof(text), "%s     %s", size, JobService::runPath());
    } else {
        snprintf(text, sizeof(text), "%s", JobService::runPath());
    }
    setText(leftDetail, text);

    bool prompt = state == JobService::RUN_PROMPT;
    show(leftHeading, prompt || state == JobService::RUN_PAUSED);
    setText(leftHeading, prompt ? "Waiting for you" : "Paused");
    if (prompt) {
        setText(leftMessage, JobService::promptText());
    } else if (JobService::active() && online) {
        setText(leftMessage, MachineService::message());
    } else if (ready) {
        setText(leftMessage, "Ready. Nothing moves until START.");
    } else {
        setText(leftMessage, "");
    }
    setTextColor(leftMessage, prompt ? UITheme::STATE_HOLD : UITheme::TEXT_LIGHT);

    // With a preview the message is the status bar's; without one it is also on the left, in
    // large type
    updatePreview(state, online);
    updateOffer();

    if (JobService::active()) updateRunGroup(state, online);
    if (ended) updateEndGroup(state);
}
