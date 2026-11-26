#ifndef LRT_FILE_SERVICE_H
#define LRT_FILE_SERVICE_H

#include <stdint.h>

// Files on the controller's SD card and the display's (specs/05-file-workflow.md,
// specs/03-screens.md section 3): lists a folder, deletes a file, and uploads
// a display SD file to the controller's uploads folder. One listing at a time;
// results are polled, as the controller answers asynchronously.
class FileService {
public:
    enum Card { CARD_CONTROLLER, CARD_DISPLAY };

    static const int NAME_LEN = 96;
    static const int PATH_LEN = 200;
    static const int MAX_ENTRIES = 300;

    struct Entry {
        char name[NAME_LEN];
        int32_t size;  // bytes; -1 for a folder
        bool dir;
    };

    // Controller folder that uploads go to (D53); FLUIDNC_UPLOAD_PATH under /sd
    static const char *const UPLOAD_FOLDER;

    // Root folder of each card, as listFolder() and paths show it
    static const char *root(Card card) { return card == CARD_CONTROLLER ? "/sd" : "/"; }

    // folder + "/" + name, without a doubled slash
    static void join(char *out, int outLen, const char *folder, const char *name);
    // The folder above, or the root
    static void parent(char *out, int outLen, Card card, const char *folder);

    // --- Listing ------------------------------------------------------------
    enum ListState { LIST_IDLE, LIST_LOADING, LIST_READY, LIST_FAILED };

    // Starts listing a folder: folders first, then files, by name (D49). The
    // display SD lists G-code files only (.gcode .gc .nc .ngc .tap).
    static void list(Card card, const char *folder);
    static ListState listState() { return listStateValue; }
    static Card listCard() { return listCardValue; }
    static const char *listFolder() { return listFolderValue; }
    static const char *listError() { return listErrorText; }
    static int count() { return entryCount; }
    static const Entry &entry(int i) { return entries[i]; }
    // More entries than MAX_ENTRIES: the rest are not listed
    static bool truncated() { return listTruncated; }
    // Goes up each time a listing ends (ready or failed)
    static uint32_t listGeneration() { return generation; }

    // --- One file's size ------------------------------------------------------
    // The size of a file on the controller's SD, from a listing of its folder that leaves the
    // picker's listing alone (Run's preview of a job started elsewhere, D72, D94). Waits for
    // another request to end first
    enum SizeState { SIZE_IDLE, SIZE_LOADING, SIZE_KNOWN, SIZE_FAILED };
    static void sizeOf(const char *path);
    static SizeState sizeState() { return sizeStateValue; }
    static const char *sizePath() { return sizePathValue; }
    static int32_t sizeResult() { return sizeValue; }
    // SIZE_FAILED: why (the controller's error line, or that the file wasn't listed)
    static const char *sizeError() { return sizeErrorText; }

    // --- Deleting -------------------------------------------------------------
    // Deletes a file, then lists its folder again. Returns false (with the
    // reason in listError()) if it could not even be tried.
    static bool remove(Card card, const char *path);

    // --- Uploading ------------------------------------------------------------
    enum UploadState {
        UPLOAD_IDLE,
        UPLOAD_CHECKING,   // is a file of that name on the controller already?
        UPLOAD_EXISTS,     // yes: upload(path, true) replaces it, cancelUpload() gives up
        UPLOAD_SENDING,
        UPLOAD_VERIFYING,  // listing the folder to compare the size (S6)
        UPLOAD_DONE,
        UPLOAD_FAILED,
    };

    // Uploads a display SD file to UPLOAD_FOLDER under its own name
    static void upload(const char *localPath, bool replace);
    static void cancelUpload();
    static UploadState uploadState() { return uploadStateValue; }
    static uint32_t uploadSent();
    static uint32_t uploadTotal() { return uploadSize; }
    static const char *uploadError() { return uploadErrorText; }
    // The file on the controller, once UPLOAD_DONE
    static const char *uploadedPath() { return uploadRemotePath; }
    // Back to UPLOAD_IDLE after DONE or FAILED
    static void clearUpload();

    // Call from the main loop
    static void poll();

    // Is the display SD there? Mounts it if it was not at boot.
    static bool displayCardReady();
    // An upload task is reading the display SD; others keep off it
    static bool displayCardBusy();

private:
    enum Request { REQUEST_NONE, REQUEST_LIST, REQUEST_CHECK, REQUEST_VERIFY, REQUEST_RATES, REQUEST_SIZE };

    static void listDisplay(const char *folder);
    static void requestController(Request request, const char *folder);
    static void onLine(const char *line);
    static void requestRate(int axis);
    static void onRateLine(const char *line);
    static void finishRequest(bool ok, const char *error);
    static int parseListing(const char *json, Entry *out, int max, bool &truncated, char *error, int errorLen);
    static void sortEntries();
    static bool findInListing(const char *name, int32_t &size);

    static Entry *entries;
    static int entryCount;
    static bool listTruncated;
    static ListState listStateValue;
    static uint32_t generation;
    static Card listCardValue;
    static char listFolderValue[PATH_LEN];
    static char listErrorText[96];

    static Request request;
    static char requestFolder[PATH_LEN];
    static uint32_t requestSinceMs;
    static bool collecting;
    static bool relistPending;
    static uint32_t relistAtMs;
    // The axis rates for the preview's estimate, asked for one at a time after a connect
    static bool ratesWanted;
    static bool wasConnected;
    static uint32_t connectedAtMs;
    static int rateAxis;

    static SizeState sizeStateValue;
    static bool sizeWanted;
    static char sizePathValue[PATH_LEN];
    static int32_t sizeValue;
    static char sizeErrorText[80];

    static UploadState uploadStateValue;
    static char uploadLocalPath[PATH_LEN];
    static char uploadName[NAME_LEN];
    static char uploadRemotePath[PATH_LEN];
    static uint32_t uploadSize;
    static char uploadErrorText[96];
};

#endif // LRT_FILE_SERVICE_H
