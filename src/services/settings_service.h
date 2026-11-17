#ifndef LRT_SETTINGS_SERVICE_H
#define LRT_SETTINGS_SERVICE_H

#include "ui/machine_config.h"

// Persistent settings (specs/02-architecture.md sections 3 and 4).
// The machine connection is shared with the classic UI: slot 0 of its
// "fluidtouch" preferences. Settings only the new UI has will live in the
// "lrt" namespace; none exist yet apart from the UI mode (app/ui_mode.h).
class SettingsService {
public:
    enum MachineStatus {
        MACHINE_OK,
        MACHINE_NOT_CONFIGURED,  // classic slot 0 is empty
        MACHINE_NOT_WIFI,        // slot 0 uses UART, USB or ESP-NOW; the new UI is WiFi only (D4)
    };

    // Reads the machine connection from classic slot 0
    static MachineStatus machine(MachineConfig &out);
};

#endif // LRT_SETTINGS_SERVICE_H
