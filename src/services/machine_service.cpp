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
uint32_t MachineService::connectAtMs = 0;
uint32_t MachineService::stateSinceMs = 0;
MachineState MachineService::lastState = STATE_DISCONNECTED;
MachineService::ProbeResult MachineService::probe = {0, 0, 0, 0, false};

void MachineService::Listener::onProbeResult(float x, float y, float z, bool success) {
    probe.count++;
    probe.x = x;
    probe.y = y;
    probe.z = z;
    probe.success = success;
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
// socket, so it is only called from here, one attempt at a time.
void MachineService::connectController() {
    connectPending = false;
    stateSinceMs = millis();
    Serial.printf("MachineService: connecting to FluidNC at %s:%d\n", config.fluidnc_url, config.websocket_port);
    if (!FluidNCClient::connect(config)) {
        retryControllerIn(RETRY_DELAY_MS);
    }
}

void MachineService::poll() {
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
    lastState = s.state;
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
    if (!online()) return;
    FluidNCClient::sendCommand("$X\n");
}

void MachineService::reconnect() {
    if (linkState == LINK_CONTROLLER) {
        if (!connectPending) FluidNCClient::disconnect();
        retryControllerIn(FIRST_CONNECT_DELAY_MS);
    } else if (linkState == LINK_WIFI) {
        WiFi.disconnect();
        startWifi();
    }
}
