#include "services/terminal_service.h"
#include "network/fluidnc_client.h"
#include "services/machine_service.h"
#include "services/macro_service.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

namespace {
const uint32_t LISTEN_MS = 3000;  // replies are kept until this long passes with no new line
bool listening = false;
}  // namespace

char TerminalService::lines[TerminalService::LINES][TerminalService::LINE_LEN];
int TerminalService::first = 0;
int TerminalService::lineCount = 0;
uint32_t TerminalService::gen = 0;
uint32_t TerminalService::listenUntil = 0;

void TerminalService::begin() { FluidNCClient::setTerminalCallback(onLine); }

bool TerminalService::canSend() { return busyReason() == nullptr; }

const char *TerminalService::busyReason() {
    if (!MachineService::online()) return "Not connected.";
    if (MachineService::jobActive()) return "Not while a job runs.";
    MacroService::RunState m = MacroService::runState();
    if (m == MacroService::RUN_STARTING || m == MacroService::RUN_RUNNING || m == MacroService::RUN_HELD)
        return "Wait for the macro to finish.";
    return nullptr;
}

bool TerminalService::send(const char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(line);
    while (n && (line[n - 1] == ' ' || line[n - 1] == '\t' || line[n - 1] == '\r' || line[n - 1] == '\n')) n--;
    if (!n || n > (size_t)COMMAND_LEN || !canSend()) return false;
    char cmd[COMMAND_LEN + 2];
    memcpy(cmd, line, n);
    cmd[n] = '\0';
    char shown[LINE_LEN];
    snprintf(shown, sizeof(shown), "> %s", cmd);
    add(shown);
    Serial.printf("TerminalService: sending %s\n", cmd);
    cmd[n] = '\n';
    cmd[n + 1] = '\0';
    FluidNCClient::sendCommand(cmd);
    listening = true;
    listenUntil = millis() + LISTEN_MS;
    return true;
}

const char *TerminalService::line(int i) {
    if (i < 0 || i >= lineCount) return "";
    return lines[(first + i) % LINES];
}

void TerminalService::add(const char *text) {
    int slot;
    if (lineCount < LINES) {
        slot = (first + lineCount++) % LINES;
    } else {
        slot = first;
        first = (first + 1) % LINES;
    }
    strncpy(lines[slot], text, LINE_LEN - 1);
    lines[slot][LINE_LEN - 1] = '\0';
    // No line ends or tabs on the screen
    for (char *c = lines[slot]; *c; c++) {
        if (*c == '\r' || *c == '\n' || *c == '\t') *c = ' ';
    }
    gen++;
}

void TerminalService::onLine(const char *payload) {
    if (!listening) return;
    if ((int32_t)(millis() - listenUntil) > 0) {
        listening = false;
        return;
    }
    if (!payload[0] || payload[0] == '<' || strncmp(payload, "[GC:", 4) == 0) return;
    add(payload);
    listenUntil = millis() + LISTEN_MS;
}
