#include "services/job_notes.h"
#include <stdlib.h>
#include <string.h>

void JobNotes::attach(void *memory, size_t bytes) {
    mem = memory;
    size = bytes;
    clear();
}

void JobNotes::clear() {
    opCount = 0;
    msgCount = 0;
    textLow = size;
    isFull = mem == nullptr || size < MAX_OPS * sizeof(Op);
}

// As FluidNC reads a line (GCode.cpp collapseGCode): a `(` starts a comment, `)` ends it, an
// unclosed one runs to the end, and `;` ends the line, inside a comment too
void JobNotes::scanLine(uint32_t line, const char *data, uint32_t length) {
    if (!memchr(data, '(', length)) return;
    const char *start = nullptr;
    for (uint32_t i = 0; i < length; i++) {
        char c = data[i];
        if (c == ';') break;
        if (c == '(') {
            start = data + i + 1;
        } else if (c == ')' && start) {
            const char *end = data + i;
            // FluidNC: a comment holding "MSG" is sent from its fifth character on
            for (const char *p = start; p + 3 <= end; p++) {
                if (p[0] == 'M' && p[1] == 'S' && p[2] == 'G') {
                    if (end - start > 4) addMessage(line, start + 4, end - start - 4);
                    break;
                }
            }
            start = nullptr;
        }
    }
    if (start) {
        const char *end = data + length;
        for (const char *p = start; p + 3 <= end; p++) {
            if (p[0] == 'M' && p[1] == 'S' && p[2] == 'G') {
                if (end - start > 4) addMessage(line, start + 4, end - start - 4);
                break;
            }
        }
    }
}

void JobNotes::addMessage(uint32_t line, const char *text, size_t length) {
    if (isFull) return;
    while (length && (*text == ' ' || *text == '\t')) {
        text++;
        length--;
    }
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r')) length--;
    if (!length) return;
    if (length > TEXT_MAX) length = TEXT_MAX;
    size_t entriesEnd = MAX_OPS * sizeof(Op) + (msgCount + 1) * sizeof(Entry);
    if (textLow < length + 1 || textLow - length - 1 < entriesEnd) {
        isFull = true;
        return;
    }
    textLow -= length + 1;
    memcpy((char *)mem + textLow, text, length);
    ((char *)mem)[textLow + length] = '\0';
    entries()[msgCount] = Entry{line, (uint32_t)textLow};
    msgCount++;
}

const char *JobNotes::messageAt(uint32_t line) const {
    // The entries are in line order: the last one at or before `line`
    uint32_t lo = 0, hi = msgCount;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (entries()[mid].line <= line) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo ? messageText(lo - 1) : nullptr;
}

namespace {

bool blank(char c) { return c == ' ' || c == '\t'; }

// `sent` matches `stored` from its `skip`-th non-blank character, blanks ignored; `sent` may have
// been cut short at TEXT_MAX
bool sameFrom(const char *stored, int skip, const char *sent) {
    const char *s = stored;
    for (; *s && skip; s++) {
        if (!blank(*s)) skip--;
    }
    if (skip) return false;
    const char *t = sent;
    for (;;) {
        while (blank(*s)) s++;
        while (blank(*t)) t++;
        if (!*t) return !*s || strlen(sent) >= JobNotes::TEXT_MAX - 8;
        if (*s != *t) return false;
        s++;
        t++;
    }
}

}  // namespace

bool JobNotes::isFileMessage(const char *text) const {
    const char *t = text;
    while (blank(*t)) t++;
    if (!*t) return false;
    for (uint32_t i = 0; i < msgCount; i++) {
        const char *stored = messageText(i);
        for (int skip = 0; skip <= 2; skip++) {
            if (sameFrom(stored, skip, t)) return true;
        }
    }
    return false;
}

void JobNotes::addOp(uint32_t start, uint32_t end, uint16_t number) {
    if (!mem || size < MAX_OPS * sizeof(Op) || opCount >= MAX_OPS || end <= start) return;
    ops()[opCount++] = Op{start, end, number, 0};
}

void JobNotes::finishOps() {
    qsort(ops(), opCount, sizeof(Op), [](const void *a, const void *b) -> int {
        uint32_t x = ((const Op *)a)->start, y = ((const Op *)b)->start;
        return x < y ? -1 : x > y ? 1 : 0;
    });
}

uint16_t JobNotes::opAt(uint32_t line) const {
    // The ranges are few (an operation's body and its fixtures): the last one starting at or
    // before `line` that holds it
    uint16_t found = 0;
    for (int i = 0; i < opCount && ops()[i].start <= line; i++) {
        if (line < ops()[i].end) found = ops()[i].number;
    }
    return found;
}
