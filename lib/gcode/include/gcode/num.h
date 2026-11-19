// Numbers (specs/04 §9, 15 §4.4, §13.2–13.3): fixed-point values, the float helpers, and number
// text. Each function mirrors one in packages/core/src/num/ and gives the same bits.
//
// Fixed-point values are integers scaled by 10^p. As in TS, they're held in a double where TS
// holds them in a `number` (positions, extents) and must be "safe integers" (|n| <= 2^53 - 1).
#pragma once

#include <cstddef>
#include <cstdint>

namespace gcode {

/** Number.MAX_SAFE_INTEGER. */
constexpr int64_t kMaxSafeInteger = 9007199254740991LL;

/** Number.isSafeInteger(x). */
bool isSafeInteger(double x);

/** 10^p as a double, exact for 0 <= p <= 22 (num/fixed.ts pow10, which allows 0..15). */
double pow10(int p);

/**
 * parseFixed / parseFixedRange: a word's source digits ("-12.5", "+.25", "3.") at precision p.
 * False when the scaled value isn't a safe integer, or the text has more fraction digits than p.
 */
bool parseFixed(const char* text, size_t length, int p, int64_t* out);

/** roundHalfAwayFromZero: sign(x) * floor(|x| + 0.5), with no -0. */
double roundHalfAwayFromZero(double x);

/** JS Math.round: ties toward +infinity, -0 kept for x in [-0.5, -0]. */
double jsRound(double x);

/** toFixed(b, p): a float bound to fixed point at p, the one place rounding happens. */
double toFixed(double b, int p);

/** toNumber(n, p): a fixed-point value as a float, always by division (15 §4.4). */
inline double toNumber(double n, int p) { return n / pow10(p); }

/** JS Number(text) for decimal text (sign, digits, optional point and digits): correctly rounded. */
double parseDecimal(const char* text, size_t length);

// ---- Number text (num/format.ts). Each writes at most `capacity` bytes (no terminator) and
// returns the length, or -1 where TS throws a RangeError or the text doesn't fit.

/** formatFixed(n, p, style): trailing fraction zeros trimmed, no exponent, -0 written 0. */
int formatFixed(double n, int p, bool leadingZero, char* out, size_t capacity);

/** formatDecimal(x, digits, keepZeros). */
int formatDecimal(double x, int digits, bool keepZeros, char* out, size_t capacity);

/** formatSignificant(x, significant). */
int formatSignificant(double x, int significant, char* out, size_t capacity);

/** formatCount(n): thousands separators, as en-US writes them. */
int formatCount(double n, char* out, size_t capacity);

// ---- Float helpers (num/float.ts): exact operations only, so every platform agrees.
namespace fmath {

/** sqrt(x*x + y*y), computed plainly. */
double hypot(double x, double y);
/** fdlibm atan2 (FreeBSD's e_atan2.c). */
double atan2(double y, double x);
/** fdlibm sin; NaN past about 2^19 * pi/2. */
double sin(double x);
/** fdlibm cos; NaN past about 2^19 * pi/2. */
double cos(double x);
/** atan2(sqrt((1 - u)(1 + u)), u). */
double acos(double u);

}  // namespace fmath
}  // namespace gcode
