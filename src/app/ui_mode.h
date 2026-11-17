#ifndef LRT_UI_MODE_H
#define LRT_UI_MODE_H

#include <Arduino.h>

// Which user interface starts at boot (specs/02-architecture.md section 3).
// Stored in NVS namespace "lrt", key "ui_mode". Only one UI is created per
// boot, so switching means saving the mode and restarting.
namespace UiMode {

enum Mode : uint8_t {
    APP = 0,      // LowRider Touch (default)
    CLASSIC = 1,  // The original FluidTouch tabs
};

Mode get();
void set(Mode mode);

// Saves the mode, shows "Restarting..." on the active screen and restarts
void restartInto(Mode mode);

}  // namespace UiMode

#endif // LRT_UI_MODE_H
