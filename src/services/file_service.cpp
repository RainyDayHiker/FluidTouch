#include "services/file_service.h"
#include "config.h"
#include "network/fluidnc_client.h"
#include "services/machine_service.h"
#include "ui/upload_manager.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <string.h>
#include <strings.h>

const char *const FileService::UPLOAD_FOLDER = "/sd" FLUIDNC_UPLOAD_PATH;  // "/sd/fluidtouch/uploads/"

FileService::Entry *FileService::entries = nullptr;
int FileService::entryCount = 0;
bool FileService::listTruncated = false;
FileService::ListState FileService::listStateValue = FileService::LIST_IDLE;
uint32_t FileService::generation = 0;
FileService::Card FileService::listCardValue = FileService::CARD_CONTROLLER;
char FileService::listFolderValue[PATH_LEN] = "/sd";
char FileService::listErrorText[96] = "";

FileService::Request FileService::request = FileService::REQUEST_NONE;
char FileService::requestFolder[PATH_LEN] = "";
uint32_t FileService::requestSinceMs = 0;
bool FileService::collecting = false;
bool FileService::relistPending = false;
uint32_t FileService::relistAtMs = 0;
bool FileService::ratesWanted = false;
bool FileService::wasConnected = false;
uint32_t FileService::connectedAtMs = 0;
int FileService::rateAxis = 0;

FileService::UploadState FileService::uploadStateValue = FileService::UPLOAD_IDLE;
FileService::SizeState FileService::sizeStateValue = FileService::SIZE_IDLE;
bool FileService::sizeWanted = false;
char FileService::sizePathValue[FileService::PATH_LEN] = "";
int32_t FileService::sizeValue = -1;
char FileService::uploadLocalPath[PATH_LEN] = "";
char FileService::uploadName[NAME_LEN] = "";
char FileService::uploadRemotePath[PATH_LEN] = "";
uint32_t FileService::uploadSize = 0;
char FileService::uploadErrorText[96] = "";

namespace {

// How long the controller may take to answer $Files/ListGcode
const uint32_t LIST_TIMEOUT_MS = 6000;
// $SD/Delete answers before the card is updated; list again a moment later
const uint32_t RELIST_DELAY_MS = 400;

// The controller's reply, collected line by line until "ok"
String jsonBuffer;

void copy(char *out, int outLen, const char *text) {
    strncpy(out, text, outLen - 1);
    out[outLen - 1] = '\0';
}

const char *baseName(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool isGcodeName(const char *name) {
    static const char *const extensions[] = {".gcode", ".gc", ".nc", ".ngc", ".tap"};
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (const char *ext : extensions) {
        if (strcasecmp(dot, ext) == 0) return true;
    }
    return false;
}

}  // namespace

// --- Paths -------------------------------------------------------------------

void FileService::join(char *out, int outLen, const char *folder, const char *name) {
    size_t n = strlen(folder);
    bool slash = n > 0 && folder[n - 1] == '/';
    snprintf(out, outLen, "%s%s%s", folder, slash ? "" : "/", name);
}

void FileService::parent(char *out, int outLen, Card card, const char *folder) {
    copy(out, outLen, folder);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';  // no trailing slash
    char *slash = strrchr(out, '/');
    if (!slash || slash == out) {
        copy(out, outLen, root(card));
        return;
    }
    *slash = '\0';
    // Not above the card's root
    if (card == CARD_CONTROLLER && strncmp(out, "/sd", 3) != 0) copy(out, outLen, root(card));
}

// --- Listing -------------------------------------------------------------------

void FileService::list(Card card, const char *folder) {
    if (!entries) {
        entries = (Entry *)heap_caps_malloc(sizeof(Entry) * MAX_ENTRIES, MALLOC_CAP_SPIRAM);
        if (!entries) {
            listStateValue = LIST_FAILED;
            generation++;
            copy(listErrorText, sizeof(listErrorText), "Out of memory");
            return;
        }
    }
    listCardValue = card;
    copy(listFolderValue, sizeof(listFolderValue), folder);
    entryCount = 0;
    listTruncated = false;
    listErrorText[0] = '\0';
    relistPending = false;
    if (card == CARD_DISPLAY) {
        listDisplay(folder);
    } else if (!FluidNCClient::isConnected()) {
        listStateValue = LIST_FAILED;
        generation++;
        copy(listErrorText, sizeof(listErrorText), "The controller is offline.");
    } else if (request != REQUEST_NONE) {
        // An upload check is under way; list once it is done
        listStateValue = LIST_LOADING;
        relistPending = true;
        relistAtMs = millis() + 200;
    } else {
        listStateValue = LIST_LOADING;
        requestController(REQUEST_LIST, folder);
    }
}

bool FileService::displayCardReady() {
    if (SD.cardType() != CARD_NONE) return true;
    // Not mounted at boot (no card then, or the auto-import did not run)
    return UploadManager::init();
}

bool FileService::displayCardBusy() { return UploadManager::isUploading(); }

void FileService::listDisplay(const char *folder) {
    if (UploadManager::isUploading()) {
        // The upload task reads the card; one user at a time
        listStateValue = LIST_FAILED;
        generation++;
        copy(listErrorText, sizeof(listErrorText), "The display SD is busy with an upload.");
        return;
    }
    if (!displayCardReady()) {
        listStateValue = LIST_FAILED;
        generation++;
        copy(listErrorText, sizeof(listErrorText), "No SD card in the display.");
        return;
    }
    File dir = SD.open(folder);
    if (!dir || !dir.isDirectory()) {
        listStateValue = LIST_FAILED;
        generation++;
        copy(listErrorText, sizeof(listErrorText), "Folder not found on the display SD.");
        return;
    }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const char *name = baseName(f.name());
        bool isDir = f.isDirectory();
        bool skip = name[0] == '.' || strcasecmp(name, "System Volume Information") == 0 ||
                    (!isDir && !isGcodeName(name));
        if (!skip) {
            if (entryCount == MAX_ENTRIES) {
                listTruncated = true;
            } else {
                Entry &e = entries[entryCount++];
                copy(e.name, sizeof(e.name), name);
                e.dir = isDir;
                e.size = isDir ? -1 : (int32_t)f.size();
            }
        }
        f.close();
    }
    dir.close();
    sortEntries();
    listStateValue = LIST_READY;
    generation++;
}

void FileService::sortEntries() {
    std::sort(entries, entries + entryCount, [](const Entry &a, const Entry &b) {
        if (a.dir != b.dir) return a.dir;
        return strcasecmp(a.name, b.name) < 0;
    });
}

// --- Controller requests -----------------------------------------------------------

void FileService::requestController(Request what, const char *folder) {
    request = what;
    copy(requestFolder, sizeof(requestFolder), folder);
    requestSinceMs = millis();
    collecting = false;
    jsonBuffer = "";
    FluidNCClient::setMessageCallback([](const char *payload) { onLine(payload); });
    char cmd[PATH_LEN + 24];
    snprintf(cmd, sizeof(cmd), "$Files/ListGcode=%s\n", folder);
    FluidNCClient::sendCommand(cmd);
}

// The axis rates (specs/11-preview.md §5): $/axes/x/max_rate_mm_per_min and so on, one at a
// time; the answer is "$/axes/x/max_rate_mm_per_min=8000.000". An error moves on to the next
void FileService::requestRate(int axis) {
    static const char names[3] = {'x', 'y', 'z'};
    request = REQUEST_RATES;
    rateAxis = axis;
    requestSinceMs = millis();
    collecting = false;
    FluidNCClient::setMessageCallback([](const char *payload) { onLine(payload); });
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "$/axes/%c/max_rate_mm_per_min\n", names[axis]);
    FluidNCClient::sendCommand(cmd);
}

void FileService::onRateLine(const char *line) {
    static const char names[3] = {'x', 'y', 'z'};
    bool answered = false;
    const char *key = strstr(line, "max_rate_mm_per_min");
    const char *axes = strstr(line, "axes/");
    if (key && axes && tolower((unsigned char)axes[5]) == names[rateAxis]) {
        const char *v = key + strlen("max_rate_mm_per_min");
        while (*v == ' ' || *v == '=' || *v == ':') v++;
        float rate = (float)atof(v);
        if (rate > 0) MachineService::setAxisRate(rateAxis, rate);
        answered = true;
    } else if (strncmp(line, "error:", 6) == 0) {
        answered = true;
    }
    if (!answered) return;
    if (rateAxis < 2) {
        requestRate(rateAxis + 1);
    } else {
        Serial.printf("FileService: axis rates %.0f %.0f %.0f mm/min\n", MachineService::axisRate(0),
                      MachineService::axisRate(1), MachineService::axisRate(2));
        finishRequest(true, nullptr);
    }
}

// Each received line; a WebSocket text frame may hold several
void FileService::onLine(const char *payload) {
    if (request == REQUEST_NONE) return;
    const char *p = payload;
    while (*p) {
        const char *nl = strchr(p, '\n');
        String line = nl ? String(p).substring(0, nl - p) : String(p);
        p = nl ? nl + 1 : p + strlen(p);
        line.trim();
        if (request == REQUEST_RATES) {
            if (line.length() && line[0] != '<') onRateLine(line.c_str());
            if (request != REQUEST_RATES) return;
            continue;
        }
        if (line.length() == 0 || line[0] == '<' || line.startsWith("[GC:") || line.startsWith("[MSG:") ||
            line.startsWith("PING")) {
            continue;
        }
        if (line.startsWith("[JSON:") || line.startsWith("{\"files") || line.startsWith("{\"error")) {
            collecting = true;
            jsonBuffer = "";
            if (line.startsWith("[JSON:")) {
                line.remove(0, 6);
                if (line.endsWith("]")) line.remove(line.length() - 1);
            }
            jsonBuffer += line;
        } else if (line.equalsIgnoreCase("ok")) {
            // An "ok" before the listing belongs to another command ($G)
            if (collecting) {
                finishRequest(true, nullptr);
                return;
            }
        } else if (line.startsWith("error:")) {
            if (!collecting) {
                finishRequest(false, line.c_str());
                return;
            }
        } else if (collecting) {
            jsonBuffer += line;  // a listing split over several lines
        }
    }
}

int FileService::parseListing(const char *json, Entry *out, int max, bool &truncated, char *error, int errorLen) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        copy(error, errorLen, "The controller's file list could not be read.");
        return -1;
    }
    if (doc["error"].is<const char *>()) {
        snprintf(error, errorLen, "%s", doc["error"].as<const char *>());
        return -1;
    }
    int n = 0;
    truncated = false;
    for (JsonObject f : doc["files"].as<JsonArray>()) {
        const char *name = f["name"] | "";
        if (!name[0]) continue;
        if (n == max) {
            truncated = true;
            break;
        }
        // The size comes as a string or a number; "-1" is a folder
        int32_t size = f["size"].is<const char *>() ? atol(f["size"].as<const char *>()) : f["size"].as<int32_t>();
        Entry &e = out[n++];
        copy(e.name, sizeof(e.name), name);
        e.dir = size < 0;
        e.size = size;
    }
    return n;
}

bool FileService::findInListing(const char *name, int32_t &size) {
    JsonDocument doc;
    if (deserializeJson(doc, jsonBuffer.c_str()) || !doc["files"].is<JsonArray>()) return false;
    for (JsonObject f : doc["files"].as<JsonArray>()) {
        if (strcmp(f["name"] | "", name) == 0) {
            size = f["size"].is<const char *>() ? atol(f["size"].as<const char *>()) : f["size"].as<int32_t>();
            return true;
        }
    }
    return false;
}

void FileService::finishRequest(bool ok, const char *error) {
    Request done = request;
    request = REQUEST_NONE;
    collecting = false;
    FluidNCClient::clearMessageCallback();

    if (done == REQUEST_LIST) {
        if (!ok) {
            listStateValue = LIST_FAILED;
            generation++;
            snprintf(listErrorText, sizeof(listErrorText), "%s", error ? error : "No answer from the controller.");
        } else {
            int n = parseListing(jsonBuffer.c_str(), entries, MAX_ENTRIES, listTruncated, listErrorText,
                                 sizeof(listErrorText));
            if (n < 0) {
                listStateValue = LIST_FAILED;
                generation++;
            } else {
                entryCount = n;
                sortEntries();
                listStateValue = LIST_READY;
                generation++;
            }
        }
    } else if (done == REQUEST_CHECK) {
        int32_t size;
        // A failed listing most likely means the folder doesn't exist yet;
        // UploadManager creates it
        if (ok && findInListing(uploadName, size)) {
            uploadStateValue = UPLOAD_EXISTS;
        } else {
            upload(uploadLocalPath, true);
        }
    } else if (done == REQUEST_SIZE) {
        int32_t size = -1;
        if (ok && findInListing(baseName(sizePathValue), size) && size >= 0) {
            sizeValue = size;
            sizeStateValue = SIZE_KNOWN;
        } else {
            sizeStateValue = SIZE_FAILED;
        }
        Serial.printf("FileService: size of %s: %ld\n", sizePathValue, (long)sizeValue);
    } else if (done == REQUEST_VERIFY) {
        int32_t size = -2;
        if (!ok || !findInListing(uploadName, size)) {
            uploadStateValue = UPLOAD_FAILED;
            copy(uploadErrorText, sizeof(uploadErrorText), "The uploaded file is not on the controller.");
        } else if ((uint32_t)size != uploadSize) {
            uploadStateValue = UPLOAD_FAILED;
            snprintf(uploadErrorText, sizeof(uploadErrorText), "The controller has %ld bytes of %lu; try again.",
                     (long)size, (unsigned long)uploadSize);
        } else {
            uploadStateValue = UPLOAD_DONE;
        }
    }
    jsonBuffer = "";
}

// --- One file's size -----------------------------------------------------------------

void FileService::sizeOf(const char *path) {
    copy(sizePathValue, sizeof(sizePathValue), path);
    sizeValue = -1;
    sizeStateValue = SIZE_LOADING;
    sizeWanted = true;  // asked for in poll(), once no other request is under way
}

// --- Deleting ------------------------------------------------------------------------

bool FileService::remove(Card card, const char *path) {
    char folder[PATH_LEN];
    parent(folder, sizeof(folder), card, path);
    if (card == CARD_DISPLAY) {
        if (!displayCardReady() || !SD.remove(path)) {
            copy(listErrorText, sizeof(listErrorText), "The file could not be deleted.");
            return false;
        }
        list(card, folder);
        return true;
    }
    if (!FluidNCClient::isConnected()) {
        copy(listErrorText, sizeof(listErrorText), "The controller is offline.");
        return false;
    }
    char cmd[PATH_LEN + 16];
    snprintf(cmd, sizeof(cmd), "$SD/Delete=%s\n", path);
    FluidNCClient::sendCommand(cmd);
    listCardValue = card;
    copy(listFolderValue, sizeof(listFolderValue), folder);
    listStateValue = LIST_LOADING;
    relistPending = true;
    relistAtMs = millis() + RELIST_DELAY_MS;
    return true;
}

// --- Uploading -----------------------------------------------------------------------

void FileService::upload(const char *localPath, bool replace) {
    if (localPath != uploadLocalPath) copy(uploadLocalPath, sizeof(uploadLocalPath), localPath);
    copy(uploadName, sizeof(uploadName), baseName(uploadLocalPath));
    join(uploadRemotePath, sizeof(uploadRemotePath), UPLOAD_FOLDER, uploadName);
    uploadErrorText[0] = '\0';

    auto fail = [](const char *text) {
        uploadStateValue = UPLOAD_FAILED;
        copy(uploadErrorText, sizeof(uploadErrorText), text);
    };
    if (!FluidNCClient::isConnected()) return fail("The controller is offline.");
    if (!displayCardReady()) return fail("No SD card in the display.");
    File f = SD.open(uploadLocalPath);
    if (!f || f.isDirectory()) return fail("The file is not on the display SD.");
    uploadSize = f.size();
    f.close();

    if (!replace) {
        if (request != REQUEST_NONE) return fail("Busy listing files; try again.");
        uploadStateValue = UPLOAD_CHECKING;
        requestController(REQUEST_CHECK, UPLOAD_FOLDER);
        return;
    }
    if (!UploadManager::startUpload(uploadLocalPath, uploadName, false, FLUIDNC_UPLOAD_PATH)) {
        return fail("The upload could not start.");
    }
    uploadStateValue = UPLOAD_SENDING;
}

void FileService::cancelUpload() {
    if (uploadStateValue == UPLOAD_SENDING) {
        UploadManager::cancel();
    } else if (uploadStateValue == UPLOAD_CHECKING || uploadStateValue == UPLOAD_VERIFYING) {
        request = REQUEST_NONE;
        FluidNCClient::clearMessageCallback();
    }
    if (uploadStateValue != UPLOAD_SENDING) uploadStateValue = UPLOAD_IDLE;
}

uint32_t FileService::uploadSent() {
    return uploadStateValue == UPLOAD_SENDING ? (uint32_t)UploadManager::state().bytesSent : 0;
}

void FileService::clearUpload() {
    if (uploadStateValue == UPLOAD_DONE || uploadStateValue == UPLOAD_FAILED || uploadStateValue == UPLOAD_EXISTS) {
        uploadStateValue = UPLOAD_IDLE;
    }
}

// --- Poll ----------------------------------------------------------------------------------

// How long after a connect the axis rates are asked for
static const uint32_t RATES_DELAY_MS = 2000;

void FileService::poll() {
    uint32_t now = millis();

    if (request != REQUEST_NONE) {
        if (!FluidNCClient::isConnected()) {
            finishRequest(false, "The controller is offline.");
        } else if (now - requestSinceMs >= LIST_TIMEOUT_MS) {
            // A reply without "ok" (or a lost one): use what arrived, if anything
            finishRequest(collecting, collecting ? nullptr : "No answer from the controller.");
        }
    }

    // After each connect, once its own commands are answered, the axis rates (after the timeout
    // check above, which reads `now` from before a request started here)
    bool connected = FluidNCClient::isConnected();
    if (connected && !wasConnected) {
        ratesWanted = true;
        connectedAtMs = now;
    }
    wasConnected = connected;
    if (ratesWanted && connected && request == REQUEST_NONE && now - connectedAtMs >= RATES_DELAY_MS) {
        ratesWanted = false;
        requestRate(0);
    }

    if (sizeWanted && request == REQUEST_NONE) {
        sizeWanted = false;
        if (!connected) {
            sizeStateValue = SIZE_FAILED;
        } else {
            char folder[PATH_LEN];
            parent(folder, sizeof(folder), CARD_CONTROLLER, sizePathValue);
            requestController(REQUEST_SIZE, folder);
        }
    }

    if (relistPending && request == REQUEST_NONE && (int32_t)(now - relistAtMs) >= 0) {
        relistPending = false;
        char folder[PATH_LEN];
        copy(folder, sizeof(folder), listFolderValue);
        list(listCardValue, folder);
    }

    if (uploadStateValue == UPLOAD_SENDING) {
        const HttpUploadState &s = UploadManager::state();
        if (s.completed) {
            if (!s.success) {
                uploadStateValue = strcmp(s.error, UploadManager::CANCELLED) == 0 ? UPLOAD_IDLE : UPLOAD_FAILED;
                copy(uploadErrorText, sizeof(uploadErrorText), s.error);
            } else if (request != REQUEST_NONE) {
                // A listing is under way; verify once it is done (next poll)
            } else {
                uploadStateValue = UPLOAD_VERIFYING;
                requestController(REQUEST_VERIFY, UPLOAD_FOLDER);
            }
        }
    }
}
