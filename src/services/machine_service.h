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
        float workZ;       // z in work coordinates, with the offset at the time of the result
        bool success;
    };

    enum Axes : uint8_t { AXIS_X = 1, AXIS_Y = 2, AXIS_Z = 4, AXES_XY = 3, AXES_XYZ = 7 };

    // Loads the machine from settings and starts connecting. The display is
    // only passed on to the screenshot server.
    static void begin(DisplayDriver *display);

    // Call every pass of the main loop. Runs FluidNCClient::loop() itself,
    // except while a connection attempt is in progress on the worker task.
    static void poll();

    static Link link() { return linkState; }
    static bool online() { return linkState == LINK_ONLINE; }
    static SettingsService::MachineStatus machineStatus() { return machineState; }
    static const char *machineName() { return config.name; }
    static const char *wifiName() { return config.ssid; }

    // True once the controller has been online in this session: the
    // difference between "connecting" and "connection lost, reconnecting"
    static bool everOnline() { return wasOnline; }

    // While link() is LINK_CONTROLLER: milliseconds until the next connection
    // attempt starts, or 0 while an attempt is under way (the connect call, or
    // the wait for the first status report after it) or retries have stopped
    static uint32_t msToNextAttempt();
    static bool attemptInProgress() {
        return linkState == LINK_CONTROLLER && !retriesStopped && msToNextAttempt() == 0;
    }
    // The automatic retries are used up; only reconnect() tries again (D46)
    static bool waitingForReconnect() { return linkState == LINK_CONTROLLER && retriesStopped; }

    // A file was running when the connection was last lost
    static bool jobRunningAtDrop() { return jobAtDrop; }

    // Last status from FluidNC. Only meaningful while online().
    static const FluidNCStatus &status() { return FluidNCClient::getStatus(); }

    // A file is running on the controller (also true during a G4 dwell,
    // when FluidNC reports Idle)
    static bool jobActive() { return online() && status().is_sd_printing; }

    static const ProbeResult &lastProbe() { return probe; }

    // A probe started by probeZ() has not reported its result yet
    static bool probeActive() { return probing; }

    // --- What may be done now (specs/03-screens.md section 2, S3) ----------
    // The actions below check these themselves and do nothing otherwise.
    static bool canHome();    // idle or in alarm, no job, nothing under way
    static bool canZero();    // idle, no job, nothing under way (also for probeZ)
    static bool canMove();    // idle or jogging, no job, nothing else under way
    static bool canUnlock() { return online(); }
    static bool canReset() { return online(); }

    // Why the can*() checks fail at the moment, as a short sentence for the
    // status bar, e.g. "Wait for the jog to finish."
    static const char *busyReason();

    // --- Actions -----------------------------------------------------------
    // Feed hold, or jog cancel while jogging (S1)
    static void stop();
    // Jog cancel (0x85): ends the joystick's jog when it is let go
    static void cancelJog();
    // Clear an alarm ($X)
    static void unlock();
    // Soft reset (0x18)
    static void reset();
    // $H; $HZ for Z only; $HX, then $HY once X has finished, for XY
    static void homeAll();
    static void homeZ();
    static void homeXY();
    // G10 L20 P0 with 0 for each axis in the mask
    static void zero(uint8_t axes);
    // $J=G91 G21 with the axes that are not 0
    static void jogBy(float dx, float dy, float dz, int feed);
    // G91, G38.2 Z-maxDistance, G0 Z+backOff once it touches, then back to
    // G90 if that was active. The result (the touch point) arrives in
    // lastProbe().
    static void probeZ(int maxDistance, int feed, float backOff);
    // Retry the connection now instead of waiting for the next attempt.
    // Does nothing while an attempt is under way.
    static void reconnect();

private:
    class Listener : public FluidNCClientListener {
    public:
        void onProbeResult(float x, float y, float z, bool success) override;
    };

    static void startWifi();
    static void connectController();
    static void connectTask(void *);
    static void retryControllerIn(uint32_t ms);
    static void scheduleRetry();     // the next delay of the back-off, or stop

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
    static int retryCount;           // retries scheduled since the last success or Reconnect
    static bool retriesStopped;      // back-off used up; waiting for Reconnect
    // A connection attempt runs on its own task because FluidNCClient::connect()
    // blocks for many seconds when the controller is off. While it runs, the
    // main loop leaves FluidNCClient alone.
    static volatile bool connectRunning;
    static volatile bool connectFinished;
    static volatile bool connectOk;
    static uint32_t connectAtMs;
    static uint32_t stateSinceMs;    // when the current wait started
    static MachineState lastState;
    static ProbeResult probe;
    static bool probing;
    // G90 is still to be sent after a probe: the probe failed into an alarm,
    // which rejects it until unlocked
    static bool restoreG90;
    // Home XY: $HY is due once the $HX cycle has been seen and has ended
    enum HomeYStage : uint8_t { HOME_Y_NONE, HOME_Y_WAIT_START, HOME_Y_WAIT_END };
    static HomeYStage homeYStage;
    static uint32_t homeYSinceMs;
};

#endif // LRT_MACHINE_SERVICE_H
