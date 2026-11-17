#ifndef LRT_SETTINGS_SERVICE_H
#define LRT_SETTINGS_SERVICE_H

#include "ui/machine_config.h"

// Persistent settings (specs/02-architecture.md sections 3 and 4).
// The machine connection, jog and probe values are shared with the classic
// UI: slot 0 of its "fluidtouch" preferences. Settings only the new UI has
// live in the "lrt" namespace (app/app_prefs.h).
class SettingsService {
public:
    enum MachineStatus {
        MACHINE_OK,
        MACHINE_NOT_CONFIGURED,  // classic slot 0 is empty
        MACHINE_NOT_WIFI,        // slot 0 uses UART, USB or ESP-NOW; the new UI is WiFi only (D4)
    };

    // Setup has room for this many steps per axis group
    static const int MAX_STEPS = 5;

    struct Jog {
        float xySteps[MAX_STEPS];  // from classic slot 0's step lists, in mm
        int xyStepCount;
        float zSteps[MAX_STEPS];
        int zStepCount;
        float xyStep;              // the selected step (D36)
        float zStep;
        int xyFeed;                // mm/min; set in Settings only (D35)
        int zFeed;
    };

    struct Probe {
        int feed;                  // mm/min
        int maxDistance;           // mm
    };

    struct LaserOffset {
        float x, y;                // mm, from the laser crosshair to the bit (D11)
    };

    // Reads the machine connection from classic slot 0
    static MachineStatus machine(MachineConfig &out);

    // Jog and probe values from classic slot 0 (classic defaults when the
    // slot is empty) and the laser offset from "lrt"
    static Jog jog();
    static Probe probe();
    static LaserOffset laserOffset();

    // Written back to classic slot 0 (D36)
    static void saveJogXYStep(float step);
    static void saveJogZStep(float step);
};

#endif // LRT_SETTINGS_SERVICE_H
