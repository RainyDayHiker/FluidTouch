#ifndef LRT_SETTINGS_SERVICE_H
#define LRT_SETTINGS_SERVICE_H

#include "ui/machine_config.h"

// Persistent settings (specs/02-architecture.md sections 3 and 4).
// The machine connection, jog and probe values are shared with the classic
// UI: one slot of its "fluidtouch" preferences (slot 0, or the one chosen on
// Settings > Machine, D57). Settings only the new UI has live in the "lrt"
// namespace (app/app_prefs.h).
//
// Values are read once and kept: every flash access flickers the panel (D44).
// The save functions are for the Settings screen only and write just the keys
// that changed (D55).
class SettingsService {
public:
    enum MachineStatus {
        MACHINE_OK,
        MACHINE_NOT_CONFIGURED,  // the chosen classic slot is empty
        MACHINE_NOT_WIFI,        // it uses UART, USB or ESP-NOW; the new UI is WiFi only (D4)
    };

    // Setup has room for this many steps per axis group
    static const int MAX_STEPS = 5;

    struct Jog {
        float xySteps[MAX_STEPS];  // from the classic slot's step lists, in mm
        int xyStepCount;
        float zSteps[MAX_STEPS];
        int zStepCount;
        float xyStep;              // the step Setup starts with (D45)
        float zStep;
        int xyFeed;                // mm/min; set in Settings only (D35)
        int zFeed;
        int xyMaxFeed;             // the XY joystick's feed at full deflection (classic "Max")
    };

    struct Probe {
        int feed;                  // mm/min
        int maxDistance;           // mm
        float backOff;             // mm raised after a touch; "lrt" probe_back (D43)
    };

    struct LaserOffset {
        float x, y;                // mm, from the laser crosshair to the bit (D11)
    };

    // Classic Settings > Power and General (PowerManager's and main.cpp's keys)
    struct Display {
        uint8_t brightness;        // percent
        uint8_t dimBrightness;     // percent
        uint32_t dimAfter;         // seconds, 0 = never
        uint32_t offAfter;
        uint32_t deepSleepAfter;
        bool rotate180;            // takes effect after a restart
    };

    // The classic slot the new UI uses (D57)
    static int machineSlot();

    // Reads the machine connection from the chosen classic slot
    static MachineStatus machine(MachineConfig &out);

    // Any classic slot, for the Settings machine picker. False when empty;
    // out then holds classic's defaults.
    static bool machineInSlot(int slot, MachineConfig &out);

    // Jog and probe values from the chosen slot (classic defaults when it is
    // empty) and the laser offset from "lrt"
    static Jog jog();
    static Probe probe();
    static LaserOffset laserOffset();
    static Display display();
    // Settings > Tools: record jobs to the display SD ("lrt" pos_log, D62)
    static bool positionLog();

    // --- Saving (Settings screen only) -----------------------------------
    // Each writes only what differs from the stored values and returns true
    // when it wrote anything.
    //
    // Stores a slot's name and connection (an empty slot becomes a WiFi
    // machine). Sets connectionChanged when this is the slot in use and its
    // WiFi, address or port changed.
    static bool saveMachine(int slot, const MachineConfig &m, bool &connectionChanged);
    // Makes slot the one the new UI uses (D57); true when it changed
    static bool useSlot(int slot);
    static bool saveJog(const Jog &j);
    static bool saveProbe(const Probe &p);
    static bool saveLaserOffset(const LaserOffset &o);
    // Also applies the brightness and timers to PowerManager
    static bool saveDisplay(const Display &d);
    static bool savePositionLog(bool on);

    // Changes with every save, so screens that copied values know to take
    // them again
    static uint32_t generation() { return gen; }

private:
    static uint32_t gen;
};

#endif // LRT_SETTINGS_SERVICE_H
