#include "app/screens/screens.h"
#include "app/fonts.h"
#include "app/frame.h"
#include "app/widgets.h"
#include "services/job_service.h"
#include "services/machine_service.h"
#include "ui/ui_theme.h"
#include <Arduino.h>
#include <string.h>

// The Run screen (specs/03-screens.md section 4). Content area 800 x 360:
//
//   x   8..464  preview (M5); until then the file and the current message
//   x 488..792  the panel: START; or progress, overrides, PAUSE and ABORT
//               (CONTINUE at a prompt); or the result
//
// With no job chosen or running, one line and a button to the Job tab.

using namespace Widgets;

namespace {

const lv_font_t *const FONT_HUGE = &lrt_font_32;
const lv_font_t *const FONT_LARGE = &lrt_font_26;
const lv_font_t *const FONT_BIG = &lrt_font_20;
const lv_font_t *const FONT_MID = &lrt_font_18;
const lv_font_t *const FONT_SMALL = &lrt_font_14;

const lv_color_t COLOR_ABORT = LV_COLOR_MAKE(0xB0, 0x20, 0x20);

const int LEFT_X = 16, LEFT_W = 440;
const int PANEL_X = 488, PANEL_W = 304;

lv_obj_t *noneGroup;
lv_obj_t *jobGroup;      // everything but noneGroup

// Left: the file and the message
lv_obj_t *leftName;
lv_obj_t *leftDetail;
lv_obj_t *leftHeading;   // "Waiting for you" at a prompt
lv_obj_t *leftMessage;

// Panel groups, one shown at a time
lv_obj_t *startGroup;
lv_obj_t *runGroup;
lv_obj_t *endGroup;

// Run group
lv_obj_t *progressBar;
lv_obj_t *percentLabel;
lv_obj_t *timesLabel;
lv_obj_t *feedRateLabel;
lv_obj_t *overrideGroup;
lv_obj_t *feedValue;
lv_obj_t *feedButtons[3];
lv_obj_t *rapidButtons[3];
lv_obj_t *continueButton;
lv_obj_t *pauseButton;
lv_obj_t *abortButton;

// End group
lv_obj_t *endTitle;
lv_obj_t *endDetail;

const int RAPID_STEPS[3] = {25, 50, 100};

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
void onAbort(lv_event_t *) { JobService::abort(); }
void onFeed(lv_event_t *e) {
    MachineService::feedOverride((MachineService::FeedOverride)(intptr_t)lv_event_get_user_data(e));
}
void onRapid(lv_event_t *e) { MachineService::rapidOverride((int)(intptr_t)lv_event_get_user_data(e)); }
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

void createLeft(lv_obj_t *parent) {
    leftName = label(parent, LEFT_X, 12, FONT_BIG, UITheme::TEXT_LIGHT, "");
    leftDetail = label(parent, LEFT_X, 42, FONT_SMALL, UITheme::TEXT_MEDIUM, "");
    leftHeading = label(parent, LEFT_X, 92, FONT_MID, UITheme::STATE_HOLD, "Waiting for you");
    leftMessage = label(parent, LEFT_X, 124, FONT_LARGE, UITheme::TEXT_LIGHT, "");
    for (lv_obj_t *l : {leftName, leftDetail, leftMessage}) lv_obj_set_width(l, LEFT_W);
    lv_label_set_long_mode(leftName, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(leftDetail, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(leftMessage, LV_LABEL_LONG_WRAP);
    divider(parent, 472, 8, 1, Frame::CONTENT_HEIGHT - 16);
}

void createStart(lv_obj_t *parent) {
    startGroup = group(parent, PANEL_X, 0, PANEL_W, Frame::CONTENT_HEIGHT);
    lv_obj_t *start = button(startGroup, 0, 96, PANEL_W, 160, LV_SYMBOL_PLAY "  START", FONT_HUGE, onStart);
    setButtonColor(start, UITheme::BTN_PLAY);
}

void createRun(lv_obj_t *parent) {
    runGroup = group(parent, PANEL_X, 0, PANEL_W, Frame::CONTENT_HEIGHT);

    progressBar = lv_bar_create(runGroup);
    lv_obj_set_pos(progressBar, 0, 12);
    lv_obj_set_size(progressBar, PANEL_W, 24);
    lv_bar_set_range(progressBar, 0, 1000);
    lv_obj_set_style_bg_color(progressBar, UITheme::BG_BUTTON, 0);
    lv_obj_set_style_bg_color(progressBar, UITheme::BG_PROGRESS, LV_PART_INDICATOR);
    percentLabel = label(runGroup, 0, 44, FONT_BIG, UITheme::TEXT_LIGHT, "");
    timesLabel = label(runGroup, 0, 72, FONT_SMALL, UITheme::TEXT_MEDIUM, "");
    feedRateLabel = label(runGroup, 0, 94, FONT_SMALL, UITheme::TEXT_MEDIUM, "");
    divider(runGroup, 0, 124, PANEL_W, 1);

    // Overrides; CONTINUE takes their place at a prompt
    overrideGroup = group(runGroup, 0, 136, PANEL_W, 116);
    label(overrideGroup, 0, 16, FONT_MID, UITheme::TEXT_MEDIUM, "Feed");
    feedButtons[0] = button(overrideGroup, 64, 0, 56, 52, "-10", FONT_MID, onFeed,
                            (void *)(intptr_t)MachineService::FEED_MINUS_10);
    feedValue = label(overrideGroup, 124, 16, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(feedValue, 56);
    lv_obj_set_style_text_align(feedValue, LV_TEXT_ALIGN_CENTER, 0);
    feedButtons[1] = button(overrideGroup, 184, 0, 56, 52, "+10", FONT_MID, onFeed,
                            (void *)(intptr_t)MachineService::FEED_PLUS_10);
    feedButtons[2] = button(overrideGroup, 248, 0, 56, 52, "100", FONT_MID, onFeed,
                            (void *)(intptr_t)MachineService::FEED_RESET);
    label(overrideGroup, 0, 76, FONT_MID, UITheme::TEXT_MEDIUM, "Rapid");
    for (int i = 0; i < 3; i++) {
        char text[8];
        snprintf(text, sizeof(text), "%d", RAPID_STEPS[i]);
        rapidButtons[i] = button(overrideGroup, 64 + i * 80, 60, 72, 52, text, FONT_MID, onRapid,
                                 (void *)(intptr_t)RAPID_STEPS[i]);
    }
    continueButton = button(runGroup, 0, 136, PANEL_W, 116, LV_SYMBOL_PLAY "  CONTINUE", FONT_HUGE, onContinue);
    setButtonColor(continueButton, UITheme::BTN_PLAY);

    divider(runGroup, 0, 264, PANEL_W, 1);
    // PAUSE and ABORT at opposite ends (S2)
    pauseButton = button(runGroup, 0, 280, 168, 72, LV_SYMBOL_PAUSE "  PAUSE", FONT_BIG, onPause);
    setButtonColor(pauseButton, UITheme::STATE_HOLD);
    abortButton = button(runGroup, 192, 280, 112, 72, "ABORT", FONT_BIG, onAbort);
    setButtonColor(abortButton, COLOR_ABORT);
}

void createEnd(lv_obj_t *parent) {
    endGroup = group(parent, PANEL_X, 0, PANEL_W, Frame::CONTENT_HEIGHT);
    endTitle = label(endGroup, 0, 40, FONT_HUGE, UITheme::STATE_IDLE, "");
    endDetail = label(endGroup, 0, 92, FONT_MID, UITheme::TEXT_LIGHT, "");
    lv_obj_set_width(endDetail, PANEL_W);
    lv_label_set_long_mode(endDetail, LV_LABEL_LONG_WRAP);
    button(endGroup, 0, 280, 148, 72, "Run again", FONT_BIG, onAgain);
    button(endGroup, 156, 280, 148, 72, "Back to Setup", FONT_MID, onBackToSetup);
}

// --- Updating -----------------------------------------------------------------------------

void updateRunGroup(JobService::RunState state, bool online) {
    const FluidNCStatus &s = MachineService::status();
    char text[96];
    char elapsed[16], left[16];

    float pct = JobService::percent();
    lv_bar_set_value(progressBar, (int32_t)(pct * 10), LV_ANIM_OFF);
    snprintf(text, sizeof(text), "%d%%", (int)pct);
    setText(percentLabel, state == JobService::RUN_STARTING ? "Starting..." : text);
    formatDuration(elapsed, sizeof(elapsed), JobService::elapsedMs());
    uint32_t leftMs = JobService::leftMs();
    if (leftMs) {
        formatDuration(left, sizeof(left), leftMs);
        snprintf(text, sizeof(text), "%s elapsed     ~%s left", elapsed, left);
    } else {
        snprintf(text, sizeof(text), "%s elapsed", elapsed);
    }
    setText(timesLabel, text);
    if (online) {
        snprintf(text, sizeof(text), "feed %d mm/min", (int)s.feed_rate);
    } else {
        snprintf(text, sizeof(text), "No connection; the job continues on the controller.");
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
    for (int i = 0; i < 3; i++) {
        setEnabled(rapidButtons[i], overrides);
        setButtonColor(rapidButtons[i], online && (int)s.rapid_override == RAPID_STEPS[i] ? UITheme::ACCENT_PRIMARY
                                                                                        : UITheme::BG_BUTTON);
    }

    bool paused = state == JobService::RUN_PAUSED;
    lv_obj_t *pauseLabel = lv_obj_get_child(pauseButton, 0);
    setText(pauseLabel, paused ? LV_SYMBOL_PLAY "  RESUME" : LV_SYMBOL_PAUSE "  PAUSE");
    setButtonColor(pauseButton, paused ? UITheme::BTN_PLAY : UITheme::STATE_HOLD);
    setEnabled(pauseButton, online && (state == JobService::RUN_RUNNING || paused));
    setEnabled(abortButton, online);
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

}  // namespace

void RunScreen::create(lv_obj_t *parent) {
    createNone(parent);
    jobGroup = group(parent, 0, 0, 800, Frame::CONTENT_HEIGHT);
    createLeft(jobGroup);
    createStart(jobGroup);
    createRun(jobGroup);
    createEnd(jobGroup);
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
        setText(leftMessage, MachineService::status().last_message);
    } else if (ready) {
        setText(leftMessage, "Ready. Nothing moves until START.");
    } else {
        setText(leftMessage, "");
    }
    setTextColor(leftMessage, prompt ? UITheme::STATE_HOLD : UITheme::TEXT_LIGHT);

    if (JobService::active()) updateRunGroup(state, online);
    if (ended) updateEndGroup(state);
}
