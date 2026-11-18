#include "services/macro_service.h"
#include "app/app_prefs.h"
#include "services/machine_service.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

const char *const MacroService::FOLDER = "/sd/fluidtouch/macros";

MacroService::Macro MacroService::macros[MacroService::SLOTS];
bool MacroService::loaded = false;
bool MacroService::changed = false;

MacroService::RunState MacroService::state = MacroService::RUN_NONE;
char MacroService::name[MacroService::NAME_LEN + 8] = "";
bool MacroService::isFile = false;
bool MacroService::stopRequested = false;
uint32_t MacroService::sinceMs = 0;
uint32_t MacroService::idleSinceMs = 0;
float MacroService::lastPercent = 0;
char MacroService::endReason[128] = "";
char MacroService::startMessage[128] = "";
bool MacroService::messageSeen = false;

namespace {

const char *const KEY = "macros";
const uint8_t BLOB_VERSION = 1;

// $SD/Run has this long to show up as a running file
const uint32_t START_TIMEOUT_MS = 8000;
// A command that leaves the controller idle this long is done
const uint32_t COMMAND_IDLE_MS = 1500;
// Idle with no SD: field this long ends a file (G4 dwells report Idle)
const uint32_t FILE_END_IDLE_MS = 1500;
// After a command has moved, idle this long ends it
const uint32_t COMMAND_END_IDLE_MS = 500;
// "done" or the reason it stopped stays this long
const uint32_t RESULT_SHOWN_MS = 10000;

void copy(char *out, size_t outLen, const char *text) {
    strncpy(out, text, outLen - 1);
    out[outLen - 1] = '\0';
}

const char *baseName(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

}  // namespace

bool MacroService::isMacroPath(const char *path) { return path && strstr(path, "/fluidtouch/macros/") != nullptr; }

// --- The list -----------------------------------------------------------------

void MacroService::load() {
    if (loaded) return;
    loaded = true;
    memset(macros, 0, sizeof(macros));
    for (int i = 0; i < SLOTS; i++) macros[i].color = i % COLORS;

    uint8_t blob[1 + sizeof(macros)];
    Preferences prefs;
    if (!prefs.begin(LRT_PREFS_NAMESPACE, true)) return;
    size_t len = prefs.getBytesLength(KEY);
    if (len == sizeof(blob) && prefs.getBytes(KEY, blob, sizeof(blob)) == sizeof(blob) && blob[0] == BLOB_VERSION) {
        memcpy(macros, blob + 1, sizeof(macros));
        for (int i = 0; i < SLOTS; i++) {
            Macro &m = macros[i];
            m.name[NAME_LEN - 1] = '\0';
            m.text[TEXT_LEN - 1] = '\0';
            if (m.kind > KIND_COMMAND) m.kind = KIND_EMPTY;
            if (m.color >= COLORS) m.color = 0;
        }
    } else if (len) {
        Serial.printf("[Macros] Stored list has an unknown format (%u bytes); starting empty\n", (unsigned)len);
    }
    prefs.end();
    Serial.printf("[Macros] %d macros\n", count());
}

const MacroService::Macro &MacroService::get(int slot) {
    load();
    return macros[slot < 0 || slot >= SLOTS ? 0 : slot];
}

int MacroService::count() {
    load();
    int n = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (macros[i].kind != KIND_EMPTY) n++;
    }
    return n;
}

void MacroService::set(int slot, const Macro &macro) {
    load();
    if (slot < 0 || slot >= SLOTS) return;
    Macro m = macro;
    m.name[NAME_LEN - 1] = '\0';
    m.text[TEXT_LEN - 1] = '\0';
    if (m.color >= COLORS) m.color = 0;
    if (memcmp(&m, &macros[slot], sizeof(m)) == 0) return;
    macros[slot] = m;
    changed = true;
}

void MacroService::clear(int slot) {
    load();
    if (slot < 0 || slot >= SLOTS) return;
    Macro m;
    memset(&m, 0, sizeof(m));
    m.color = macros[slot].color;
    set(slot, m);
}

void MacroService::swap(int a, int b) {
    load();
    if (a < 0 || a >= SLOTS || b < 0 || b >= SLOTS || a == b) return;
    Macro t = macros[a];
    macros[a] = macros[b];
    macros[b] = t;
    changed = true;
}

bool MacroService::save() {
    if (!loaded || !changed) return false;
    uint8_t blob[1 + sizeof(macros)];
    blob[0] = BLOB_VERSION;
    memcpy(blob + 1, macros, sizeof(macros));
    Preferences prefs;
    if (!prefs.begin(LRT_PREFS_NAMESPACE, false)) return false;
    bool ok = prefs.putBytes(KEY, blob, sizeof(blob)) == sizeof(blob);
    prefs.end();
    if (ok) changed = false;
    Serial.printf("[Macros] Saved %d macros%s\n", count(), ok ? "" : " (failed)");
    return ok;
}

// --- Running ------------------------------------------------------------------

bool MacroService::canRun() { return !active() && MachineService::canZero(); }

void MacroService::enter(RunState next) {
    if (next == RUN_STARTING || next == RUN_RUNNING) {
        if (state < RUN_STARTING || state > RUN_HELD) {
            lastPercent = 0;
            endReason[0] = '\0';
            copy(startMessage, sizeof(startMessage), MachineService::status().last_message);
            messageSeen = false;
        }
    }
    if (next == RUN_DONE || next == RUN_STOPPED) stopRequested = false;
    state = next;
    sinceMs = millis();
    idleSinceMs = 0;
}

bool MacroService::run(int slot) {
    load();
    if (slot < 0 || slot >= SLOTS || !canRun()) return false;
    const Macro &m = macros[slot];
    bool ok = false;
    if (m.kind == KIND_FILE && m.text[0]) {
        char path[TEXT_LEN + 32];
        snprintf(path, sizeof(path), "%s/%s", FOLDER, m.text);
        ok = MachineService::runFile(path);
        isFile = true;
    } else if (m.kind == KIND_COMMAND && m.text[0]) {
        ok = MachineService::sendLine(m.text);
        isFile = false;
    }
    if (!ok) return false;
    copy(name, sizeof(name), m.name[0] ? m.name : m.text);
    stopRequested = false;
    state = RUN_NONE;  // a new run: enter() takes the message and clears the rest
    enter(RUN_STARTING);
    Serial.printf("[Macros] Run %d: %s\n", slot + 1, m.text);
    return true;
}

void MacroService::resume() {
    if (state == RUN_HELD) MachineService::resume();
}

void MacroService::stop() {
    if (!active()) return;
    stopRequested = true;
    MachineService::reset();
}

const char *MacroService::message() {
    const char *now = MachineService::status().last_message;
    if (!messageSeen && strcmp(now, startMessage) == 0) return "";
    messageSeen = true;
    return now;
}

float MacroService::percent() { return state == RUN_DONE && isFile ? 100.0f : lastPercent; }

void MacroService::poll() {
    if (!MachineService::online()) return;
    const FluidNCStatus &s = MachineService::status();
    uint32_t now = millis();
    bool macroFile = s.is_sd_printing && isMacroPath(s.sd_filename);
    if (macroFile) lastPercent = s.sd_percent;
    bool idle = s.state == STATE_IDLE && !s.is_sd_printing;

    switch (state) {
        case RUN_DONE:
        case RUN_STOPPED:
            if (now - sinceMs >= RESULT_SHOWN_MS) enter(RUN_NONE);
            // fall through: a macro file started elsewhere
        case RUN_NONE:
            if (macroFile) {
                // Its name in the list, or the file name
                const char *file = baseName(s.sd_filename);
                copy(name, sizeof(name), file);
                for (int i = 0; i < SLOTS; i++) {
                    if (macros[i].kind == KIND_FILE && strcmp(macros[i].text, file) == 0 && macros[i].name[0]) {
                        copy(name, sizeof(name), macros[i].name);
                        break;
                    }
                }
                isFile = true;
                enter(RUN_RUNNING);
            }
            break;

        case RUN_STARTING:
            if (s.state == STATE_ALARM) {
                copy(endReason, sizeof(endReason), s.alarm_message[0] ? s.alarm_message : "Alarm.");
                enter(RUN_STOPPED);
            } else if (isFile) {
                if (s.is_sd_printing || s.state == STATE_RUN || s.state == STATE_HOLD || s.state == STATE_HOME) {
                    enter(RUN_RUNNING);
                } else if (now - sinceMs >= START_TIMEOUT_MS) {
                    snprintf(endReason, sizeof(endReason), "It did not start. %s", s.last_message);
                    enter(RUN_STOPPED);
                }
            } else if (!idle) {
                enter(s.state == STATE_HOLD || s.state == STATE_DOOR ? RUN_HELD : RUN_RUNNING);
            } else if (now - sinceMs >= COMMAND_IDLE_MS) {
                enter(RUN_DONE);  // a command that doesn't move, e.g. G10 L20
            }
            break;

        case RUN_RUNNING:
        case RUN_HELD:
            if (s.state == STATE_ALARM) {
                if (stopRequested) {
                    copy(endReason, sizeof(endReason), "Stopped.");
                } else {
                    copy(endReason, sizeof(endReason), s.alarm_message[0] ? s.alarm_message : "Alarm.");
                }
                enter(RUN_STOPPED);
            } else if (s.state == STATE_HOLD || s.state == STATE_DOOR) {
                if (state != RUN_HELD) enter(RUN_HELD);
            } else if (idle && stopRequested) {
                // A reset at a hold leaves the controller idle, not in alarm
                copy(endReason, sizeof(endReason), "Stopped.");
                enter(RUN_STOPPED);
            } else if (idle) {
                if (!idleSinceMs) {
                    idleSinceMs = now;
                } else if (now - idleSinceMs >= (isFile ? FILE_END_IDLE_MS : COMMAND_END_IDLE_MS)) {
                    enter(RUN_DONE);
                }
            } else {
                if (state != RUN_RUNNING) enter(RUN_RUNNING);
                idleSinceMs = 0;
            }
            break;
    }
}
