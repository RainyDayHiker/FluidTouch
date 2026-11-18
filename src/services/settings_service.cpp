#include "services/settings_service.h"
#include "app/app_prefs.h"
#include "config.h"
#include "core/power_manager.h"
#include <Preferences.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t SettingsService::gen = 1;

namespace {

// Classic defaults (ui/machine_config.cpp), used when the slot is empty
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

// {100, 50, 10, 1, 0.1} -> "100,50,10,1,0.1"
void formatSteps(char *out, size_t outLen, const float *steps, int count) {
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < count && used < outLen; i++) {
        used += snprintf(out + used, outLen - used, i ? ",%g" : "%g", steps[i]);
    }
}

bool same(float a, float b) { return fabsf(a - b) < 0.0005f; }

// Every flash access (read or write) briefly stops the RGB panel from
// reading its picture from PSRAM, which shows as a flicker. So the slot and
// the "lrt" values are read once and kept, and nothing on the working
// screens writes (D44).
int slotIndex = -1;
MachineConfig current;
bool currentLoaded = false;
bool currentConfigured = false;

struct LrtValues {
    float laserX, laserY, probeBack;
};
LrtValues lrt;
bool lrtLoaded = false;

SettingsService::Display displayValues;
bool displayLoaded = false;

int loadSlotIndex() {
    if (slotIndex < 0) {
        Preferences prefs;
        prefs.begin(LRT_PREFS_NAMESPACE, true);
        slotIndex = prefs.getInt("machine_slot", 0);
        prefs.end();
        if (slotIndex < 0 || slotIndex >= MAX_MACHINES) slotIndex = 0;
    }
    return slotIndex;
}

const MachineConfig *loadCurrent() {
    if (!currentLoaded) {
        currentLoaded = true;
        currentConfigured = MachineConfigManager::getMachine(loadSlotIndex(), current) && current.is_configured;
        if (!currentConfigured) current = MachineConfig();
    }
    return currentConfigured ? &current : nullptr;
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

// Writes into one namespace; opened on the first write only
class Writer {
public:
    explicit Writer(const char *ns) : ns(ns) {}
    ~Writer() {
        if (open) prefs.end();
    }
    Preferences &p() {
        if (!open) {
            open = true;
            prefs.begin(ns, false);
        }
        written = true;
        return prefs;
    }
    bool wrote() const { return written; }

private:
    const char *ns;
    Preferences prefs;
    bool open = false;
    bool written = false;
};

// "m2_" + "ssid"
const char *key(char *buf, size_t len, int slot, const char *name) {
    snprintf(buf, len, "m%d_%s", slot, name);
    return buf;
}

}  // namespace

int SettingsService::machineSlot() { return loadSlotIndex(); }

SettingsService::MachineStatus SettingsService::machine(MachineConfig &out) {
    const MachineConfig *m = loadCurrent();
    if (!m) return MACHINE_NOT_CONFIGURED;
    out = *m;
    // Messages name the machine ("No answer from ...")
    if (!out.name[0]) snprintf(out.name, sizeof(out.name), "Machine %d", loadSlotIndex() + 1);
    if (out.connection_type != CONN_WIFI) {
        return MACHINE_NOT_WIFI;
    }
    return MACHINE_OK;
}

bool SettingsService::machineInSlot(int slot, MachineConfig &out) {
    if (slot == loadSlotIndex()) {
        loadCurrent();
        out = current;
        return currentConfigured;
    }
    if (MachineConfigManager::getMachine(slot, out) && out.is_configured) return true;
    out = MachineConfig();
    return false;
}

SettingsService::Jog SettingsService::jog() {
    Jog j;
    const MachineConfig *m = loadCurrent();
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
    const MachineConfig *m = loadCurrent();
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

// The stored values: PowerManager read its keys at boot, before anything
// could change them; the rotation is read here, once
SettingsService::Display SettingsService::display() {
    if (!displayLoaded) {
        displayLoaded = true;
        Preferences prefs;
        prefs.begin(PREFS_SYSTEM_NAMESPACE, true);
        displayValues.rotate180 = prefs.getUChar("display_rot", 0) == 2;  // 2 = 180 degrees
        prefs.end();
        displayValues.brightness = PowerManager::getNormalBrightness();
        displayValues.dimBrightness = PowerManager::getDimBrightness();
        displayValues.dimAfter = PowerManager::getDimTimeout();
        displayValues.offAfter = PowerManager::getSleepTimeout();
        displayValues.deepSleepAfter = PowerManager::getDeepSleepTimeout();
    }
    return displayValues;
}

// --- Saving ------------------------------------------------------------------

bool SettingsService::saveMachine(int slot, const MachineConfig &m, bool &connectionChanged) {
    connectionChanged = false;
    if (slot < 0 || slot >= MAX_MACHINES) return false;
    bool wrote = false;

    MachineConfig stored;
    bool storedConfigured = machineInSlot(slot, stored);
    char k[16];
    {
        Writer w(PREFS_NAMESPACE);
        if (!storedConfigured) {
            // A new machine: every key, so nothing left over from an old one
            // shows through. Jog and probe values are classic's defaults.
            MachineConfig fresh = m;
            fresh.is_configured = true;
            fresh.connection_type = CONN_WIFI;
            Preferences &p = w.p();
            p.putString(key(k, sizeof(k), slot, "name"), fresh.name);
            p.putUChar(key(k, sizeof(k), slot, "type"), (uint8_t)CONN_WIFI);
            p.putString(key(k, sizeof(k), slot, "ssid"), fresh.ssid);
            p.putString(key(k, sizeof(k), slot, "pwd"), fresh.password);
            p.putString(key(k, sizeof(k), slot, "url"), fresh.fluidnc_url);
            p.putUShort(key(k, sizeof(k), slot, "port"), fresh.websocket_port);
            p.putFloat(key(k, sizeof(k), slot, "jxy_st"), fresh.jog_xy_step);
            p.putFloat(key(k, sizeof(k), slot, "jz_st"), fresh.jog_z_step);
            p.putInt(key(k, sizeof(k), slot, "jxy_fd"), fresh.jog_xy_feed);
            p.putInt(key(k, sizeof(k), slot, "jz_fd"), fresh.jog_z_feed);
            p.putInt(key(k, sizeof(k), slot, "jxy_mx"), fresh.jog_max_xy_feed);
            p.putString(key(k, sizeof(k), slot, "jxy_sts"), fresh.jog_xy_steps);
            p.putString(key(k, sizeof(k), slot, "jz_sts"), fresh.jog_z_steps);
            p.putInt(key(k, sizeof(k), slot, "p_feed"), fresh.probe_feed_rate);
            p.putInt(key(k, sizeof(k), slot, "p_dist"), fresh.probe_max_distance);
            p.putBool(key(k, sizeof(k), slot, "cfg"), true);
            stored = fresh;
            connectionChanged = true;
        } else {
            if (strcmp(stored.name, m.name) != 0) {
                w.p().putString(key(k, sizeof(k), slot, "name"), m.name);
                strcpy(stored.name, m.name);
            }
            if (strcmp(stored.ssid, m.ssid) != 0) {
                w.p().putString(key(k, sizeof(k), slot, "ssid"), m.ssid);
                strcpy(stored.ssid, m.ssid);
                connectionChanged = true;
            }
            if (strcmp(stored.password, m.password) != 0) {
                w.p().putString(key(k, sizeof(k), slot, "pwd"), m.password);
                strcpy(stored.password, m.password);
                connectionChanged = true;
            }
            if (strcmp(stored.fluidnc_url, m.fluidnc_url) != 0) {
                w.p().putString(key(k, sizeof(k), slot, "url"), m.fluidnc_url);
                strcpy(stored.fluidnc_url, m.fluidnc_url);
                connectionChanged = true;
            }
            if (stored.websocket_port != m.websocket_port) {
                w.p().putUShort(key(k, sizeof(k), slot, "port"), m.websocket_port);
                stored.websocket_port = m.websocket_port;
                connectionChanged = true;
            }
        }
        wrote = w.wrote();
    }
    if (slot != loadSlotIndex()) connectionChanged = false;  // not the one in use
    if (!wrote) return false;
    MachineConfigManager::reloadMachines();  // classic's copy is stale now
    if (slot == slotIndex) {
        loadCurrent();
        current = stored;
        currentConfigured = true;
    }
    gen++;
    return true;
}

bool SettingsService::useSlot(int slot) {
    if (slot < 0 || slot >= MAX_MACHINES || slot == loadSlotIndex()) return false;
    Writer w(LRT_PREFS_NAMESPACE);
    w.p().putInt("machine_slot", slot);
    slotIndex = slot;
    currentLoaded = false;  // read again from classic's copy when next asked for
    gen++;
    return true;
}

bool SettingsService::saveJog(const Jog &j) {
    if (!loadCurrent()) return false;  // stored with a machine; none yet
    int slot = loadSlotIndex();
    char k[16];
    char xy[64], z[64];
    formatSteps(xy, sizeof(xy), j.xySteps, j.xyStepCount);
    formatSteps(z, sizeof(z), j.zSteps, j.zStepCount);
    bool wrote;
    {
        Writer w(PREFS_NAMESPACE);
        if (strcmp(current.jog_xy_steps, xy) != 0) {
            w.p().putString(key(k, sizeof(k), slot, "jxy_sts"), xy);
            strcpy(current.jog_xy_steps, xy);
        }
        if (strcmp(current.jog_z_steps, z) != 0) {
            w.p().putString(key(k, sizeof(k), slot, "jz_sts"), z);
            strcpy(current.jog_z_steps, z);
        }
        if (!same(current.jog_xy_step, j.xyStep)) {
            w.p().putFloat(key(k, sizeof(k), slot, "jxy_st"), j.xyStep);
            current.jog_xy_step = j.xyStep;
        }
        if (!same(current.jog_z_step, j.zStep)) {
            w.p().putFloat(key(k, sizeof(k), slot, "jz_st"), j.zStep);
            current.jog_z_step = j.zStep;
        }
        if (current.jog_xy_feed != j.xyFeed) {
            w.p().putInt(key(k, sizeof(k), slot, "jxy_fd"), j.xyFeed);
            current.jog_xy_feed = j.xyFeed;
        }
        if (current.jog_z_feed != j.zFeed) {
            w.p().putInt(key(k, sizeof(k), slot, "jz_fd"), j.zFeed);
            current.jog_z_feed = j.zFeed;
        }
        if (current.jog_max_xy_feed != j.xyMaxFeed) {
            w.p().putInt(key(k, sizeof(k), slot, "jxy_mx"), j.xyMaxFeed);
            current.jog_max_xy_feed = j.xyMaxFeed;
        }
        wrote = w.wrote();
    }
    if (wrote) {
        MachineConfigManager::reloadMachines();
        gen++;
    }
    return wrote;
}

bool SettingsService::saveProbe(const Probe &p) {
    bool wrote = false;
    const MachineConfig *m = loadCurrent();
    if (m) {
        int slot = loadSlotIndex();
        char k[16];
        Writer w(PREFS_NAMESPACE);
        if (current.probe_feed_rate != p.feed) {
            w.p().putInt(key(k, sizeof(k), slot, "p_feed"), p.feed);
            current.probe_feed_rate = p.feed;
        }
        if (current.probe_max_distance != p.maxDistance) {
            w.p().putInt(key(k, sizeof(k), slot, "p_dist"), p.maxDistance);
            current.probe_max_distance = p.maxDistance;
        }
        if (w.wrote()) {
            wrote = true;
            MachineConfigManager::reloadMachines();
        }
    }
    loadLrt();
    if (!same(lrt.probeBack, p.backOff)) {
        Writer w(LRT_PREFS_NAMESPACE);
        w.p().putFloat("probe_back", p.backOff);
        lrt.probeBack = p.backOff;
        wrote = true;
    }
    if (wrote) gen++;
    return wrote;
}

bool SettingsService::saveLaserOffset(const LaserOffset &o) {
    loadLrt();
    Writer w(LRT_PREFS_NAMESPACE);
    if (!same(lrt.laserX, o.x)) {
        w.p().putFloat("laser_x", o.x);
        lrt.laserX = o.x;
    }
    if (!same(lrt.laserY, o.y)) {
        w.p().putFloat("laser_y", o.y);
        lrt.laserY = o.y;
    }
    if (w.wrote()) gen++;
    return w.wrote();
}

bool SettingsService::saveDisplay(const Display &d) {
    Display old = display();
    // Applied in this order so classic's rules (screen off >= dim + 10 s,
    // deep sleep >= screen off + 60 s) hold at every step; PowerManager may
    // move a later timer, and what it ends up with is what is stored
    PowerManager::setNormalBrightness(d.brightness);
    PowerManager::setDimBrightness(d.dimBrightness);
    PowerManager::setDimTimeout(d.dimAfter);
    PowerManager::setSleepTimeout(d.offAfter);
    PowerManager::setDeepSleepTimeout(d.deepSleepAfter);
    Display now;
    now.brightness = PowerManager::getNormalBrightness();
    now.dimBrightness = PowerManager::getDimBrightness();
    now.dimAfter = PowerManager::getDimTimeout();
    now.offAfter = PowerManager::getSleepTimeout();
    now.deepSleepAfter = PowerManager::getDeepSleepTimeout();
    now.rotate180 = d.rotate180;
    displayValues = now;

    Writer w(PREFS_SYSTEM_NAMESPACE);
    if (now.brightness != old.brightness) w.p().putUChar("pm_norm_bri", now.brightness);
    if (now.dimBrightness != old.dimBrightness) w.p().putUChar("pm_dim_bri", now.dimBrightness);
    if (now.dimAfter != old.dimAfter) w.p().putUInt("pm_dim_to", now.dimAfter);
    if (now.offAfter != old.offAfter) w.p().putUInt("pm_sleep_to", now.offAfter);
    if (now.deepSleepAfter != old.deepSleepAfter) w.p().putUInt("pm_deepsleep", now.deepSleepAfter);
    if (now.rotate180 != old.rotate180) w.p().putUChar("display_rot", now.rotate180 ? 2 : 0);
    if (w.wrote()) gen++;
    return w.wrote();
}
