#ifndef LRT_TERMINAL_SERVICE_H
#define LRT_TERMINAL_SERVICE_H

#include <stdint.h>

// Settings > Tools > Terminal (specs/03-screens.md section 6, D105): a line typed on the
// display sent to FluidNC as it is, and FluidNC's replies to it. Replies are kept from a
// send until 3 s pass with no new line, so the client's own polls and their answers mostly
// stay out; status reports (`<...>`) and parser states (`[GC:...]`) never show.
class TerminalService {
public:
    static const int LINES = 6;      // kept, oldest dropped first
    static const int LINE_LEN = 96;  // a longer line is cut
    static const int COMMAND_LEN = 96;

    // Takes FluidNCClient's terminal callback (the classic Terminal tab's, unused here)
    static void begin();

    // Online, and no job or macro running; otherwise why not, for the status bar (D38)
    static bool canSend();
    static const char *busyReason();

    // Sends the line (trimmed) with a newline; false when it can't now or is empty
    static bool send(const char *line);

    // The kept lines, oldest first: the sent line as "> line", then the replies
    static int count() { return lineCount; }
    static const char *line(int i);

    // Changes when a line is added, to redraw only then
    static uint32_t generation() { return gen; }

private:
    static void onLine(const char *payload);
    static void add(const char *text);

    static char lines[LINES][LINE_LEN];
    static int first, lineCount;
    static uint32_t gen;
    static uint32_t listenUntil;
};

#endif // LRT_TERMINAL_SERVICE_H
