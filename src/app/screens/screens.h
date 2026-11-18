#ifndef LRT_SCREENS_H
#define LRT_SCREENS_H

#include <lvgl.h>

// One of the five workflow screens shown in the frame's content area
// (specs/03-screens.md). Screens get their data from the services.
class Screen {
public:
    virtual ~Screen() {}

    // Text on the screen's tab
    virtual const char *title() const = 0;

    // Build the widgets inside parent (800 x 376, created once at start)
    virtual void create(lv_obj_t *parent) = 0;

    // Called about ten times a second while the screen is visible
    virtual void update() {}

    // Called when the screen becomes the visible one
    virtual void onShow() {}
};

// A centred title and a line of text, for screens that are not built yet
void makePlaceholder(lv_obj_t *parent, const char *title, const char *text);

// Home, zero, jog, laser, probe Z (specs/03-screens.md section 2). Its
// widgets and state live in screen_setup.cpp; there is one instance.
class SetupScreen : public Screen {
public:
    const char *title() const override { return "Setup"; }
    void create(lv_obj_t *parent) override;
    void update() override;
};

// The chosen file, its file picker, and Use this job (specs/03-screens.md
// section 3). Widgets and state live in screen_job.cpp.
class JobScreen : public Screen {
public:
    const char *title() const override { return "Job"; }
    void create(lv_obj_t *parent) override;
    void update() override;
};

// Start panel, progress, prompts, pause, abort, overrides (specs/03-screens.md
// section 4). Widgets and state live in screen_run.cpp.
class RunScreen : public Screen {
public:
    const char *title() const override { return "Run"; }
    void create(lv_obj_t *parent) override;
    void update() override;
};

class MacrosScreen : public Screen {
public:
    const char *title() const override { return "Macros"; }
    void create(lv_obj_t *parent) override;
};

class SettingsScreen : public Screen {
public:
    const char *title() const override { return "Settings"; }
    void create(lv_obj_t *parent) override;
};

#endif // LRT_SCREENS_H
