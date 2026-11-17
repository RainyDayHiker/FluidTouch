#include "ui/classic_client_listener.h"
#include "ui/ui_common.h"
#include "ui/tabs/control/ui_tab_control_probe.h"

ClassicClientListener* ClassicClientListener::instance() {
    static ClassicClientListener listener;
    return &listener;
}

void ClassicClientListener::onConnectionEstablished() {
    UICommon::hideConnectingPopup();
    // Also hide the error dialog if showing (connection succeeded after an error)
    UICommon::hideConnectionErrorDialog();
}

void ClassicClientListener::onConnectionLost(const char* title, const char* message) {
    UICommon::showConnectionErrorDialog(title, message);
}

void ClassicClientListener::onProbeResult(float x, float y, float z, bool success) {
    UITabControlProbe::updateResult(x, y, z, success);
}
