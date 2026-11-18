#ifndef LRT_ENTRY_H
#define LRT_ENTRY_H

#include <stddef.h>

// Pop-ups for typing a value (specs/03-screens.md section 6, D56). Both
// cover the whole screen and take every touch until closed. One is open at a
// time; done is called on OK only.
namespace Entry {

typedef void (*NumberDone)(float value, void *ctx);
typedef void (*TextDone)(const char *text, void *ctx);

// A keypad. decimals 0 allows whole numbers only; a negative min allows "-".
// A value outside min..max is refused on OK, with the range shown in red.
void number(const char *title, float value, float min, float max, int decimals, const char *unit,
            NumberDone done, void *ctx);

// A keyboard with a one-line text field, at most maxLen characters
void text(const char *title, const char *value, size_t maxLen, TextDone done, void *ctx);

// "12.5" for 12.5 with up to 3 decimals; "3000" for 3000 with 0
void formatNumber(char *out, size_t outLen, float value, int decimals);

}  // namespace Entry

#endif // LRT_ENTRY_H
