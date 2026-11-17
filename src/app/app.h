#ifndef LRT_APP_H
#define LRT_APP_H

class DisplayDriver;

// LowRider Touch: the workflow-ordered interface. main.cpp starts either this
// or the classic UI, depending on UiMode.
class LrtApp {
public:
    // Builds the UI. Call once from setup() after the display, touch, power
    // manager and FluidNCClient are initialised.
    static void begin(DisplayDriver *display);

    // True once begin() has run; main.cpp's loop() then only calls loop()
    static bool isRunning() { return running; }

    // One pass of the main loop: network, services, LVGL
    static void loop();

private:
    static bool running;
};

#endif // LRT_APP_H
