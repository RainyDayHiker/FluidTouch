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
    probeCount = homeCount = promptCount = 0;
    workCount = 0;
    textLow = size;
    isFull = mem == nullptr || size < ENTRIES_AT;
}

namespace {
bool isBlank(char c) { return c == ' ' || c == '\t'; }
}  // namespace

// As FluidNC reads a line: a `$` line is a system command (`$H`, `$HZ` home); otherwise comments
// are left out (`(` to `)`, `;` to the end) and so are blanks, and each letter is followed by a
// number. `G38` with any decimal probes; `M0` (also `M00`) is a program pause, a prompt
JobNotes::Work JobNotes::classify(const char *d, uint32_t n) {
    uint32_t i = 0;
    while (i < n && isBlank(d[i])) i++;
    if (i < n && d[i] == '$') return i + 1 < n && (d[i + 1] == 'H' || d[i + 1] == 'h') ? HOME : NONE;
    Work found = NONE;
    bool comment = false;
    while (i < n) {
        char c = d[i];
        if (comment) {
            if (c == ')') comment = false;
            if (c == ';') break;
            i++;
            continue;
        }
        if (c == '(') {
            comment = true;
            i++;
            continue;
        }
        if (c == ';') break;
        bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        i++;
        if (!letter) continue;
        char word = (char)(c & ~0x20);
        // Its number: blanks allowed anywhere, a sign first, digits with one point
        while (i < n && isBlank(d[i])) i++;
        if (i < n && (d[i] == '-' || d[i] == '+')) i++;
        bool digits = false, point = false, fraction = false;
        uint32_t whole = 0;
        while (i < n) {
            char k = d[i];
            if (isBlank(k)) {
                i++;
            } else if (k >= '0' && k <= '9') {
                digits = true;
                if (point) {
                    if (k != '0') fraction = true;
                } else if (whole < 100000) {
                    whole = whole * 10 + (uint32_t)(k - '0');
                }
                i++;
            } else if (k == '.' && !point) {
                point = true;
                i++;
            } else {
                break;
            }
        }
        if (!digits) continue;
        if (word == 'G' && whole == 38) found = PROBE;
        if (word == 'M' && whole == 0 && !fraction && found == NONE) found = PROMPT;
    }
    return found;
}

double JobNotes::workSecondsAfter(uint32_t line) const {
    double s = 0;
    for (int i = workCount - 1; i >= 0 && work()[i].line > line; i--) s += work()[i].kind == PROBE ? PROBE_S : HOME_S;
    return s;
}

bool JobNotes::workBetween(uint32_t after, uint32_t upTo) const {
    for (int i = 0; i < workCount; i++) {
        if (work()[i].line > upTo) break;
        if (work()[i].line > after) return true;
    }
    return false;
}

// As FluidNC reads a line (GCode.cpp collapseGCode): a `(` starts a comment, `)` ends it, an
// unclosed one runs to the end, and `;` ends the line, inside a comment too
void JobNotes::scanLine(uint32_t line, const char *data, uint32_t length) {
    Work w = classify(data, length);
    if (w == PROMPT) promptCount++;
    if (w == PROBE || w == HOME) {
        if (w == PROBE) probeCount++;
        if (w == HOME) homeCount++;
        if (mem && size >= ENTRIES_AT && workCount < MAX_WORK) work()[workCount++] = WorkLine{line, (uint8_t)w, {0, 0, 0}};
    }
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
    size_t entriesEnd = ENTRIES_AT + (msgCount + 1) * sizeof(Entry);
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
