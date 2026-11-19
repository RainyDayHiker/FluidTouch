#pragma once
#include <stddef.h>

// Text from outside the program (file names, FluidNC messages, and later the G-code library's
// names and summaries) as LVGL can show it (specs/04 §3, D71).
namespace AppText {

// True when s is valid UTF-8: no stray continuation bytes, no overlong forms, no surrogates,
// nothing above U+10FFFF.
bool validUtf8(const char *s);

// Text for a label. Valid UTF-8 comes back unchanged (s itself). Otherwise every byte is read
// as Latin-1, as the G-code Editor reads a file that isn't valid UTF-8, and written to buf as
// UTF-8; the C1 controls (0x80..0x9F), which no font has, become '?'. Cut at a character
// boundary if buf is too small.
const char *forDisplay(const char *s, char *buf, size_t size);

}  // namespace AppText
