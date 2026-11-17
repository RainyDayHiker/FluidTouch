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

// Every flash access (read or write) briefly stops the RGB panel from
// reading its picture from PSRAM, which shows as a flicker. So slot 0 and the
// "lrt" values are read once and kept, and nothing on the screens writes
// (D44).
namespace {

MachineConfig slot0;
bool slot0Loaded = false;
bool slot0Configured = false;

struct LrtValues {
    float laserX, laserY, probeBack;
};
LrtValues lrt;
bool lrtLoaded = false;

const MachineConfig *loadSlot0() {
    if (!slot0Loaded) {
        slot0Loaded = true;
        slot0Configured = MachineConfigManager::getMachine(0, slot0) && slot0.is_configured;
    }
    return slot0Configured ? &slot0 : nullptr;
}

const LrtValues &loadLrt() {
    if (!lrtLoaded) {
        lrtLoaded = true;
        Preferences prefs;
        prefs.begin(LRT_PREFS_NAMESPACE, true);
        // Laser defaults from laser_offset.gcode (D11); the probe back-off is
        // the new UI's own, not classic's retract (2 mm by default) (D43)
        lrt.laserX = prefs.getFloat("laser_x", 33.6f);
        lrt.laserY = prefs.getFloat("laser_y", -28.2f);
        lrt.probeBack = prefs.getFloat("probe_back", 10.0f);
        prefs.end();
    }
    return lrt;
}

}  // namespace

SettingsService::MachineStatus SettingsService::machine(MachineConfig &out) {
    const MachineConfig *m = loadSlot0();
    if (!m) return MACHINE_NOT_CONFIGURED;
    out = *m;
    if (out.connection_type != CONN_WIFI) {
        return MACHINE_NOT_WIFI;
    }
    return MACHINE_OK;
}

SettingsService::Jog SettingsService::jog() {
    Jog j;
    const MachineConfig *m = loadSlot0();
    if (m) {
        stepsOrDefault(m->jog_xy_steps, DEFAULT_XY_STEPS, j.xySteps, j.xyStepCount);
        stepsOrDefault(m->jog_z_steps, DEFAULT_Z_STEPS, j.zSteps, j.zStepCount);
        j.xyStep = m->jog_xy_step;
        j.zStep = m->jog_z_step;
        j.xyFeed = m->jog_xy_feed > 0 ? m->jog_xy_feed : 3000;
        j.zFeed = m->jog_z_feed > 0 ? m->jog_z_feed : 1000;
        j.xyMaxFeed = m->jog_max_xy_feed > 0 ? m->jog_max_xy_feed : 3000;
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
    Probe p = {100, 10, loadLrt().probeBack};
    const MachineConfig *m = loadSlot0();
    if (m) {
        if (m->probe_feed_rate > 0) p.feed = m->probe_feed_rate;
        if (m->probe_max_distance > 0) p.maxDistance = m->probe_max_distance;
    }
    return p;
}

SettingsService::LaserOffset SettingsService::laserOffset() {
    const LrtValues &v = loadLrt();
    return {v.laserX, v.laserY};
}
