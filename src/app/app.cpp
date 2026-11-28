#include "app/app.h"
#include "app/frame.h"
#include "app/splash.h"
#include <Arduino.h>
#include <lvgl.h>
#include "core/display_driver.h"
#include "core/power_manager.h"
#include "network/fluidnc_client.h"
#include "network/screenshot_server.h"
#include "services/file_service.h"
#include "services/job_service.h"
#include "services/machine_service.h"
#include "services/macro_service.h"
#include "services/position_log.h"
#include "services/preview_service.h"
#include "services/run_follow.h"
#include "services/run_preview.h"
#include "services/terminal_service.h"

bool LrtApp::running = false;

void LrtApp::begin(DisplayDriver *display) {
    Serial.println("LrtApp: starting LowRider Touch");

    // The splash first (D104), then the frame is built under it, so the screen
    // is never blank while WiFi and FluidNC are being reached
    Splash::show();
    MachineService::begin(display);
    TerminalService::begin();
    Frame::create();
    lv_timer_handler();

    running = true;
}

void LrtApp::loop() {
    ScreenshotServer::handleClient();
    MachineService::poll();  // also runs FluidNCClient::loop()
    FileService::poll();
    JobService::poll();
    PreviewService::poll();
    RunFollow::poll();
    RunPreview::poll();
    MacroService::poll();
    PositionLog::poll();
    Splash::poll();

    uint32_t now = millis();

    static uint32_t lastFrameUpdate = 0;
    if (now - lastFrameUpdate >= 100) {
        lastFrameUpdate = now;
        Frame::update();
    }

    static uint32_t lastPowerUpdate = 0;
    if (now - lastPowerUpdate >= 250) {
        lastPowerUpdate = now;
        // A running job keeps the display awake, also through a G4 dwell
        int state = STATE_IDLE;
        if (MachineService::jobActive()) {
            state = STATE_RUN;
        } else if (MachineService::online()) {
            state = MachineService::status().state;
        }
        PowerManager::update(state);
    }

    static uint32_t lastTick = 0;
    lv_tick_inc(now - lastTick);
    lastTick = now;

    lv_timer_handler();
    delay(5);
}
