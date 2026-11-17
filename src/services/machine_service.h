#ifndef LRT_MACHINE_SERVICE_H
#define LRT_MACHINE_SERVICE_H

#include "network/fluidnc_client.h"
#include "services/settings_service.h"

class DisplayDriver;

// Owns the connection to the one machine and is the only place the new UI
// talks to FluidNCClient (specs/02-architecture.md section 4). Screens read
// link() and status() and call the action functions; they never parse
// FluidNC text. Everything here runs on the main loop.
class MachineService {
public:
    enum Link : uint8_t {
        LINK_NO_MACHINE,   // nothing usable in classic slot 0; see machineStatus()
        LINK_WIFI,         // joining the WiFi network
        LINK_CONTROLLER,   // WiFi is up; connecting to FluidNC (or waiting to retry)
        LINK_ONLINE,       // status reports are arriving
    };

    struct ProbeResult {
        uint32_t count;    // increments with every [PRB:...] received; 0 = none yet
        float x, y, z;     // machine coordinates
        bool success;
    };

    // Loads the machine from settings and starts connecting. The display is
    // only passed on to the screenshot server.
    static void begin(DisplayDriver *display);

    // Call every pass of the main loop, after FluidNCClient::loop()
    static void poll();

    static Link link() { return linkState; }
    static bool online() { return linkState == LINK_ONLINE; }
    static SettingsService::MachineStatus machineStatus() { return machineState; }
    static const char *machineName() { return config.name; }
    static const char *wifiName() { return config.ssid; }

    // True once the controller has been online in this session: the
    // difference between "connecting" and "connection lost, reconnecting"
    static bool everOnline() { return wasOnline; }

    // A file was running when the connection was last lost
    static bool jobRunningAtDrop() { return jobAtDrop; }

    // Last status from FluidNC. Only meaningful while online().
    static const FluidNCStatus &status() { return FluidNCClient::getStatus(); }

    // A file is running on the controller (also true during a G4 dwell,
    // when FluidNC reports Idle)
    static bool jobActive() { return online() && status().is_sd_printing; }

    static const ProbeResult &lastProbe() { return probe; }

    // --- Actions -----------------------------------------------------------
    // Feed hold, or jog cancel while jogging (S1)
    static void stop();
    // Clear an alarm ($X)
    static void unlock();
    // Retry the connection now instead of waiting for the next attempt
    static void reconnect();

private:
    class Listener : public FluidNCClientListener {
    public:
        void onProbeResult(float x, float y, float z, bool success) override;
    };

    static void startWifi();
    static void connectController();
    static void retryControllerIn(uint32_t ms);

    static Listener listener;
    static DisplayDriver *display;
    static MachineConfig config;
    static SettingsService::MachineStatus machineState;
    static Link linkState;
    static bool wasOnline;
    static bool jobAtDrop;
    static bool lastJobActive;
    static bool networkServicesStarted;
    static bool connectPending;      // connectController() is due at connectAtMs
    static uint32_t connectAtMs;
    static uint32_t stateSinceMs;    // when the current wait started
    static MachineState lastState;
    static ProbeResult probe;
};

#endif // LRT_MACHINE_SERVICE_H
