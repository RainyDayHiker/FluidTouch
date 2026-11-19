#include "app/text.h"
#include <stdint.h>

namespace AppText {

bool validUtf8(const char *s) {
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint8_t c = *p;
        if (c < 0x80) {
            p++;
            continue;
        }
        int n;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) {
            n = 1;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            n = 2;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            n = 3;
            cp = c & 0x07;
        } else {
            return false;  // a continuation byte, 0xC0/0xC1 (always overlong), or above 0xF4
        }
        for (int i = 1; i <= n; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;  // also stops at the terminating 0
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        p += n + 1;
    }
    return true;
}

const char *forDisplay(const char *s, char *buf, size_t size) {
    if (!s) return "";
    if (validUtf8(s)) return s;
    if (size == 0) return "";
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        uint8_t c = *p;
        if (c < 0x80) {
            if (o + 1 >= size) break;
            buf[o++] = (char)c;
        } else if (c < 0xA0) {
            if (o + 1 >= size) break;
            buf[o++] = '?';
        } else {
            if (o + 2 >= size) break;
            buf[o++] = (char)(0xC0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    buf[o] = '\0';
    return buf;
}

}  // namespace AppText
