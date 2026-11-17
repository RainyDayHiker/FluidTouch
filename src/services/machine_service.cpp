#include "services/machine_service.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include "core/display_driver.h"
#include "network/screenshot_server.h"

// Timing of the connection state machine
static const uint32_t WIFI_TIMEOUT_MS = 15000;        // then WiFi.begin() again
static const uint32_t CONTROLLER_TIMEOUT_MS = 10000;  // no status report after connecting
static const uint32_t RETRY_DELAY_MS = 5000;          // between controller attempts
static const uint32_t FIRST_CONNECT_DELAY_MS = 150;   // lets the frame draw "Connecting" first

MachineService::Listener MachineService::listener;
DisplayDriver *MachineService::display = nullptr;
MachineConfig MachineService::config;
SettingsService::MachineStatus MachineService::machineState = SettingsService::MACHINE_NOT_CONFIGURED;
MachineService::Link MachineService::linkState = MachineService::LINK_NO_MACHINE;
bool MachineService::wasOnline = false;
bool MachineService::jobAtDrop = false;
bool MachineService::lastJobActive = false;
bool MachineService::networkServicesStarted = false;
bool MachineService::connectPending = false;
volatile bool MachineService::connectRunning = false;
volatile bool MachineService::connectFinished = false;
volatile bool MachineService::connectOk = false;
uint32_t MachineService::connectAtMs = 0;
uint32_t MachineService::stateSinceMs = 0;
MachineState MachineService::lastState = STATE_DISCONNECTED;
MachineService::ProbeResult MachineService::probe = {0, 0, 0, 0, 0, false};
bool MachineService::probing = false;
bool MachineService::restoreG90 = false;

void MachineService::Listener::onProbeResult(float x, float y, float z, bool success) {
    probe.count++;
    probe.x = x;
    probe.y = y;
    probe.z = z;
    probe.workZ = z - FluidNCClient::getStatus().wco_z;
    probe.success = success;
    probing = false;
    // After a miss FluidNC is in alarm and would reject G90; unlock() sends it
    if (success && restoreG90) {
        FluidNCClient::sendCommand("G90\n");
        restoreG90 = false;
    }
}

void MachineService::begin(DisplayDriver *displayDriver) {
    display = displayDriver;
    FluidNCClient::setListener(&listener);

    machineState = SettingsService::machine(config);
    if (machineState != SettingsService::MACHINE_OK) {
        Serial.printf("MachineService: no usable machine in classic slot 0 (status %d)\n", (int)machineState);
        linkState = LINK_NO_MACHINE;
        return;
    }

    Serial.printf("MachineService: machine '%s' at %s:%d, WiFi '%s'\n",
                  config.name, config.fluidnc_url, config.websocket_port, config.ssid);
    startWifi();
}

void MachineService::startWifi() {
    linkState = LINK_WIFI;
    stateSinceMs = millis();
    connectPending = false;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(config.ssid, config.password);
}

void MachineService::retryControllerIn(uint32_t ms) {
    linkState = LINK_CONTROLLER;
    connectPending = true;
    connectAtMs = millis() + ms;
}

// FluidNCClient::connect() blocks while it resolves the name and opens the
// socket: 10 s and more when the controller is switched off (measured on the
// device). So each attempt runs on a task of its own on core 0, and until it
// has finished nothing else touches FluidNCClient (see poll()).
void MachineService::connectTask(void *) {
    connectOk = FluidNCClient::connect(config);
    connectFinished = true;
    vTaskDelete(nullptr);
}

void MachineService::connectController() {
    connectPending = false;
    stateSinceMs = millis();
    Serial.printf("MachineService: connecting to FluidNC at %s:%d\n", config.fluidnc_url, config.websocket_port);
    connectFinished = false;
    connectRunning = true;
    if (xTaskCreatePinnedToCore(connectTask, "lrt_connect", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        // No task: do it here, at the price of a frozen screen
        Serial.println("MachineService: could not start the connect task");
        connectOk = FluidNCClient::connect(config);
        connectFinished = true;
    }
}

void MachineService::poll() {
    if (connectRunning) {
        if (!connectFinished) return;  // the task owns FluidNCClient for now
        connectRunning = false;
        stateSinceMs = millis();       // the wait for the first status report starts here
        if (!connectOk) retryControllerIn(RETRY_DELAY_MS);
    }
    FluidNCClient::loop();

    if (linkState == LINK_NO_MACHINE) return;

    uint32_t now = millis();
    bool wifiUp = WiFi.status() == WL_CONNECTED;

    if (linkState == LINK_WIFI) {
        if (wifiUp) {
            Serial.printf("MachineService: WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
            if (!networkServicesStarted) {
                networkServicesStarted = true;
                // The mDNS client is needed to resolve names such as fluidnc.local
                if (!MDNS.begin("fluidtouch")) {
                    Serial.println("MachineService: mDNS failed to start; .local names will not resolve");
                }
                if (display) ScreenshotServer::init(display);
            }
            retryControllerIn(FIRST_CONNECT_DELAY_MS);
        } else if (now - stateSinceMs >= WIFI_TIMEOUT_MS) {
            Serial.println("MachineService: WiFi not connected yet, trying again");
            WiFi.disconnect();
            startWifi();
        }
        return;
    }

    if (!wifiUp) {
        // The network went away; the controller connection goes with it
        Serial.println("MachineService: WiFi lost");
        if (linkState == LINK_ONLINE) jobAtDrop = lastJobActive;
        probing = false;
        FluidNCClient::disconnect();
        startWifi();
        return;
    }

    if (linkState == LINK_CONTROLLER) {
        if (FluidNCClient::isConnected()) {
            Serial.println("MachineService: online");
            linkState = LINK_ONLINE;
            wasOnline = true;
            jobAtDrop = false;
            lastState = STATE_DISCONNECTED;
        } else if (connectPending) {
            if ((int32_t)(now - connectAtMs) >= 0) connectController();
        } else if (now - stateSinceMs >= CONTROLLER_TIMEOUT_MS) {
            Serial.println("MachineService: no status from FluidNC, trying again");
            FluidNCClient::disconnect();
            retryControllerIn(RETRY_DELAY_MS);
        }
        return;
    }

    // LINK_ONLINE
    if (!FluidNCClient::isConnected()) {
        Serial.println("MachineService: connection to FluidNC lost");
        jobAtDrop = lastJobActive;
        probing = false;
        FluidNCClient::disconnect();
        retryControllerIn(RETRY_DELAY_MS);
        return;
    }

    const FluidNCStatus &s = FluidNCClient::getStatus();
    lastJobActive = s.is_sd_printing;
    if (lastState == STATE_ALARM && s.state != STATE_ALARM) {
        // The alarm text belongs to the alarm that has just been cleared
        FluidNCClient::clearAlarmMessage();
    }
    if (s.state == STATE_ALARM) probing = false;  // the [PRB:] line normally comes first
    lastState = s.state;
}

uint32_t MachineService::msToNextAttempt() {
    if (linkState != LINK_CONTROLLER || !connectPending) return 0;
    int32_t remaining = (int32_t)(connectAtMs - millis());
    // Under half a second counts as "now": the first attempt is scheduled a
    // moment ahead only to let the screen draw first
    return remaining > 500 ? (uint32_t)remaining : 0;
}

void MachineService::stop() {
    if (!online()) return;
    if (status().state == STATE_JOG) {
        FluidNCClient::sendCommand("\x85");  // jog cancel
    } else {
        FluidNCClient::sendCommand("!");     // feed hold
    }
}

void MachineService::unlock() {
    if (!canUnlock()) return;
    FluidNCClient::sendCommand("$X\n");
    if (restoreG90) {
        FluidNCClient::sendCommand("G90\n");
        restoreG90 = false;
    }
}

// --- What may be done now ---------------------------------------------------

namespace {
// FluidNC reports Idle during a G4 dwell inside a running file, so a job
// counts as busy whatever the state says
bool idleNoJob(const FluidNCStatus &s) { return s.state == STATE_IDLE && !s.is_sd_printing; }
}  // namespace

bool MachineService::canHome() {
    if (!online() || probing) return false;
    const FluidNCStatus &s = status();
    return !s.is_sd_printing && (s.state == STATE_IDLE || s.state == STATE_ALARM);
}

bool MachineService::canZero() {
    return online() && !probing && idleNoJob(status());
}

bool MachineService::canMove() {
    if (!online() || probing) return false;
    const FluidNCStatus &s = status();
    return !s.is_sd_printing && (s.state == STATE_IDLE || s.state == STATE_JOG);
}

bool MachineService::canUnlock() {
    if (!online()) return false;
    const FluidNCStatus &s = status();
    return s.state == STATE_ALARM || s.is_sd_printing || (s.state == STATE_IDLE && !probing);
}

// --- Actions ----------------------------------------------------------------

void MachineService::reset() {
    if (!canReset()) return;
    FluidNCClient::sendCommand("\x18");
    // A reset ends a probe and puts the modal state back to its defaults (G90)
    probing = false;
    restoreG90 = false;
}

void MachineService::homeAll() {
    if (canHome()) FluidNCClient::sendCommand("$H\n");
}

void MachineService::homeZ() {
    if (canHome()) FluidNCClient::sendCommand("$HZ\n");
}

void MachineService::zero(uint8_t axes) {
    if (!canZero() || !(axes & AXES_XYZ)) return;
    char cmd[40] = "G10 L20 P0";
    if (axes & AXIS_X) strcat(cmd, " X0");
    if (axes & AXIS_Y) strcat(cmd, " Y0");
    if (axes & AXIS_Z) strcat(cmd, " Z0");
    strcat(cmd, "\n");
    FluidNCClient::sendCommand(cmd);
}

void MachineService::jogBy(float dx, float dy, float dz, int feed) {
    if (!canMove() || feed <= 0 || (dx == 0 && dy == 0 && dz == 0)) return;
    char cmd[96];
    int n = snprintf(cmd, sizeof(cmd), "$J=G91 G21");
    if (dx != 0) n += snprintf(cmd + n, sizeof(cmd) - n, " X%.3f", dx);
    if (dy != 0) n += snprintf(cmd + n, sizeof(cmd) - n, " Y%.3f", dy);
    if (dz != 0) n += snprintf(cmd + n, sizeof(cmd) - n, " Z%.3f", dz);
    snprintf(cmd + n, sizeof(cmd) - n, " F%d\n", feed);
    FluidNCClient::sendCommand(cmd);
}

void MachineService::goToXYZero(int feed) {
    if (!canMove() || feed <= 0) return;
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "$J=G90 G21 X0 Y0 F%d\n", feed);
    FluidNCClient::sendCommand(cmd);
}

void MachineService::probeZ(int maxDistance, int feed) {
    if (!canZero() || maxDistance <= 0 || feed <= 0) return;
    // G91 goes on a line of its own, as in the classic UI's probe tab
    bool wasG91 = strcmp(status().modal_distance, "G91") == 0;
    FluidNCClient::sendCommand("G91\n");
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "G38.2 Z-%d F%d\n", maxDistance, feed);
    FluidNCClient::sendCommand(cmd);
    probing = true;
    restoreG90 = !wasG91;  // sent when the result arrives
}

void MachineService::reconnect() {
    if (connectRunning) return;  // an attempt is already under way
    if (linkState == LINK_CONTROLLER) {
        if (!connectPending) return;  // waiting for the first status report of an attempt
        retryControllerIn(FIRST_CONNECT_DELAY_MS);
    } else if (linkState == LINK_WIFI) {
        WiFi.disconnect();
        startWifi();
    }
}
