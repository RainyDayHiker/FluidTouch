#ifndef LRT_FRAME_H
#define LRT_FRAME_H

#include <lvgl.h>

class Screen;

// The fixed parts of the new UI (specs/03-screens.md section 1):
//
//   tabs        800 x 56   Setup | Job | Run | Macros | Settings
//   content     800 x 376  one screen at a time
//   status bar  800 x 64   state | positions over the last message | WiFi, STOP
//
// An alarm banner or an offline banner covers the status bar when needed.
class Frame {
public:
    enum ScreenId { SCREEN_SETUP, SCREEN_JOB, SCREEN_RUN, SCREEN_MACROS, SCREEN_SETTINGS, SCREEN_COUNT };

    static const int TAB_HEIGHT = 56;
    static const int STATUS_HEIGHT = 64;
    static const int CONTENT_HEIGHT = 480 - TAB_HEIGHT - STATUS_HEIGHT;  // 360

    // Shows a short message of the display's own (e.g. why a press was not
    // sent) in the status bar's message row for a few seconds, or until
    // FluidNC sends a message
    static void notice(const char *text);

    // Builds the frame and all screens and loads it as the active LVGL screen
    static void create();

    // Refreshes the status bar, banners and the visible screen from the services
    static void update();

    static void show(ScreenId id);
    static ScreenId current() { return currentId; }

private:
    static void createTabs(lv_obj_t *root);
    static void createStatusBar(lv_obj_t *root);
    static void createBanner(lv_obj_t *root);
    static void updateStatusBar();
    static void updateBanner();
    static void onTabClicked(lv_event_t *e);
    static void onStopClicked(lv_event_t *e);
    static void onBannerButtonClicked(lv_event_t *e);

    static ScreenId currentId;
};

#endif // LRT_FRAME_H
