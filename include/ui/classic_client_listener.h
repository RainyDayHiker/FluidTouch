#ifndef CLASSIC_CLIENT_LISTENER_H
#define CLASSIC_CLIENT_LISTENER_H

#include "network/fluidnc_client.h"

// The classic UI's reaction to FluidNCClient events: connection popups and the
// probe tab's result display. FluidNCClient used to call these UI functions
// directly; it now reports events to whichever UI registered a listener.
class ClassicClientListener : public FluidNCClientListener {
public:
    static ClassicClientListener* instance();

    void onConnectionEstablished() override;
    void onConnectionLost(const char* title, const char* message) override;
    void onProbeResult(float x, float y, float z, bool success) override;
};

#endif // CLASSIC_CLIENT_LISTENER_H
