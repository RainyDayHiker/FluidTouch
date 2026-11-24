#pragma once

#include <stddef.h>
#include <stdint.h>

// What Run shows beside the tool's position, taken from the file during the analysis
// (specs/11-preview.md §6, D92): the `(MSG ...)` comments with their lines, and the operations'
// line ranges for "op N of M". Plain C++ with no Arduino calls, for the host test.
//
// One block of memory owned by the caller: the operations at the bottom (MAX_OPS of them),
// then the messages' entries growing up and their text growing down. When it is full, later
// messages are left out (full() says so) and the controller's own message is shown for them.
class JobNotes {
public:
    static const int MAX_OPS = 256;
    static const size_t TEXT_MAX = 127;  // as FluidNCStatus::last_message holds

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

private:
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
    Entry *entries() const { return (Entry *)((uint8_t *)mem + MAX_OPS * sizeof(Op)); }
    void addMessage(uint32_t line, const char *text, size_t length);

    void *mem = nullptr;
    size_t size = 0;
    int opCount = 0;
    uint32_t msgCount = 0;
    size_t textLow = 0;  // the lowest byte of text used: entries may grow up to here
    bool isFull = false;
};
