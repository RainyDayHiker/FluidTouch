#pragma once

#include <stddef.h>
#include <stdint.h>

// What Run shows beside the tool's position, taken from the file during the analysis
// (specs/11-preview.md §6, D92): the `(MSG ...)` comments with their lines, and the operations'
// line ranges for "op N of M". Plain C++ with no Arduino calls, for the host test.
//
// Also the probes, homings and prompts, for the estimate's allowances (§5, D101, D102).
//
// One block of memory owned by the caller: the operations at the bottom (MAX_OPS of them), the
// probes' and homings' lines (MAX_WORK), then the messages' entries growing up and their text
// growing down. When it is full, later messages are left out (full() says so) and the
// controller's own message is shown for them.
class JobNotes {
public:
    static const int MAX_OPS = 256;
    static const int MAX_WORK = 128;     // probes and homings whose lines are kept
    static const size_t TEXT_MAX = 127;  // as FluidNCStatus::last_message holds
    // Allowances in seconds (D101, D102): what the plan has no time for
    static constexpr double PROBE_S = 20;
    static constexpr double HOME_S = 10;
    static constexpr double PROMPT_S = 45;

    void attach(void *memory, size_t bytes);
    void clear();
    bool attached() const { return mem != nullptr; }

    // Packing: every line of the file, in order
    void scanLine(uint32_t line, const char *data, uint32_t length);
    // After the analysis: an enabled operation's line range [start, end), numbered from 1
    void addOp(uint32_t start, uint32_t end, uint16_t number);
    // After the last addOp: sorts the ranges
    void finishOps();

    uint32_t messages() const { return msgCount; }
    bool full() const { return isFull; }
    uint32_t messageLine(uint32_t i) const { return entries()[i].line; }
    const char *messageText(uint32_t i) const { return (const char *)mem + entries()[i].text; }
    // The last message at or before `line`, or null
    const char *messageAt(uint32_t line) const;
    // `text` (a message as the controller sent it) is one of the file's messages. FluidNC sends a
    // comment holding "MSG" from its fifth character on; a version that drops the comment's
    // spaces first shifts that by one, so the comparison ignores spaces and allows the shift
    bool isFileMessage(const char *text) const;

    // The operation `line` is in, 0 for none
    uint16_t opAt(uint32_t line) const;

    // Probes (G38.x), homings ($H...) and prompts (M0) in the whole file
    uint32_t probes() const { return probeCount; }
    uint32_t homings() const { return homeCount; }
    uint32_t prompts() const { return promptCount; }
    // The probes' and homings' allowances in the whole file, and on lines after `line`
    double workSeconds() const { return probeCount * PROBE_S + homeCount * HOME_S; }
    double workSecondsAfter(uint32_t line) const;
    // A probe or a homing on a line in (after, upTo]
    bool workBetween(uint32_t after, uint32_t upTo) const;
    // The line's G-code words, as FluidNC reads them: a probe, a homing, a prompt or nothing
    enum Work : uint8_t { NONE = 0, PROBE = 1, HOME = 2, PROMPT = 3 };
    static Work classify(const char *data, uint32_t length);

private:
    struct WorkLine {
        uint32_t line;
        uint8_t kind;  // PROBE or HOME
        uint8_t pad[3];
    };
    struct Op {
        uint32_t start, end;
        uint16_t number;
        uint16_t pad;
    };
    struct Entry {
        uint32_t line;
        uint32_t text;  // offset of its text in the block
    };
    Op *ops() const { return (Op *)mem; }
    WorkLine *work() const { return (WorkLine *)((uint8_t *)mem + MAX_OPS * sizeof(Op)); }
    static const size_t ENTRIES_AT = MAX_OPS * sizeof(Op) + MAX_WORK * sizeof(WorkLine);
    Entry *entries() const { return (Entry *)((uint8_t *)mem + ENTRIES_AT); }
    void addMessage(uint32_t line, const char *text, size_t length);

    void *mem = nullptr;
    size_t size = 0;
    int opCount = 0;
    uint32_t probeCount = 0, homeCount = 0, promptCount = 0;
    int workCount = 0;  // lines kept in work()
    uint32_t msgCount = 0;
    size_t textLow = 0;  // the lowest byte of text used: entries may grow up to here
    bool isFull = false;
};
