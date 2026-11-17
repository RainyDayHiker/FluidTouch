#include "services/settings_service.h"

SettingsService::MachineStatus SettingsService::machine(MachineConfig &out) {
    if (!MachineConfigManager::getMachine(0, out) || !out.is_configured) {
        return MACHINE_NOT_CONFIGURED;
    }
    if (out.connection_type != CONN_WIFI) {
        return MACHINE_NOT_WIFI;
    }
    return MACHINE_OK;
}
