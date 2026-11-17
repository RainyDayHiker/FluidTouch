#include "services/settings_service.h"
#include "app/app_prefs.h"
#include <Preferences.h>
#include <stdlib.h>
#include <string.h>

namespace {

// Classic defaults (ui/machine_config.cpp), used when slot 0 is empty
const char *const DEFAULT_XY_STEPS = "100,50,10,1,0.1";
const char *const DEFAULT_Z_STEPS = "50,25,10,1,0.1";

// "100,50,10,1,0.1" -> {100, 50, 10, 1, 0.1}; at most max values, positive only
int parseSteps(const char *text, float *out, int max) {
    int count = 0;
    const char *p = text;
    while (*p && count < max) {
        char *end;
        float value = strtof(p, &end);
        if (end == p) break;
        if (value > 0) out[count++] = value;
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    return count;
}

void stepsOrDefault(const char *text, const char *fallback, float *out, int &count) {
    count = parseSteps(text, out, SettingsService::MAX_STEPS);
    if (count == 0) count = parseSteps(fallback, out, SettingsService::MAX_STEPS);
}

}  // namespace

SettingsService::MachineStatus SettingsService::machine(MachineConfig &out) {
    if (!MachineConfigManager::getMachine(0, out) || !out.is_configured) {
        return MACHINE_NOT_CONFIGURED;
    }
    if (out.connection_type != CONN_WIFI) {
        return MACHINE_NOT_WIFI;
    }
    return MACHINE_OK;
}

SettingsService::Jog SettingsService::jog() {
    Jog j;
    MachineConfig m;
    if (MachineConfigManager::getMachine(0, m) && m.is_configured) {
        stepsOrDefault(m.jog_xy_steps, DEFAULT_XY_STEPS, j.xySteps, j.xyStepCount);
        stepsOrDefault(m.jog_z_steps, DEFAULT_Z_STEPS, j.zSteps, j.zStepCount);
        j.xyStep = m.jog_xy_step;
        j.zStep = m.jog_z_step;
        j.xyFeed = m.jog_xy_feed > 0 ? m.jog_xy_feed : 3000;
        j.zFeed = m.jog_z_feed > 0 ? m.jog_z_feed : 1000;
        j.xyMaxFeed = m.jog_max_xy_feed > 0 ? m.jog_max_xy_feed : 3000;
    } else {
        stepsOrDefault("", DEFAULT_XY_STEPS, j.xySteps, j.xyStepCount);
        stepsOrDefault("", DEFAULT_Z_STEPS, j.zSteps, j.zStepCount);
        j.xyStep = 10.0f;
        j.zStep = 1.0f;
        j.xyFeed = 3000;
        j.zFeed = 1000;
        j.xyMaxFeed = 3000;
    }
    return j;
}

SettingsService::Probe SettingsService::probe() {
    Probe p = {100, 10};
    MachineConfig m;
    if (MachineConfigManager::getMachine(0, m) && m.is_configured) {
        if (m.probe_feed_rate > 0) p.feed = m.probe_feed_rate;
        if (m.probe_max_distance > 0) p.maxDistance = m.probe_max_distance;
    }
    return p;
}

SettingsService::LaserOffset SettingsService::laserOffset() {
    Preferences prefs;
    prefs.begin(LRT_PREFS_NAMESPACE, true);
    // Defaults from laser_offset.gcode (D11)
    LaserOffset o = {prefs.getFloat("laser_x", 33.6f), prefs.getFloat("laser_y", -28.2f)};
    prefs.end();
    return o;
}

void SettingsService::saveJogXYStep(float step) {
    MachineConfig m;
    if (!MachineConfigManager::getMachine(0, m) || !m.is_configured || m.jog_xy_step == step) return;
    m.jog_xy_step = step;
    MachineConfigManager::saveMachine(0, m);
}

void SettingsService::saveJogZStep(float step) {
    MachineConfig m;
    if (!MachineConfigManager::getMachine(0, m) || !m.is_configured || m.jog_z_step == step) return;
    m.jog_z_step = step;
    MachineConfigManager::saveMachine(0, m);
}
