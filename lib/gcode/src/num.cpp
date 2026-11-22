// Fixed point, rounding, decimal text and number text: packages/core/src/num/fixed.ts and
// format.ts, line for line where the arithmetic matters (specs/15 §4.4, §13.3).
#include "gcode/num.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "gcode/version.h"

namespace gcode {

const char* version() { return GCODE_CORE_VERSION; }

namespace {

// Exact powers of ten, by repeated multiplication (fixed.ts's table).
struct Pow10Table {
  double v[23];
  Pow10Table() {
    v[0] = 1;
    for (int i = 1; i < 23; i++) v[i] = v[i - 1] * 10;
  }
};
const Pow10Table kPow10;

}  // namespace

double pow10(int p) { return (p >= 0 && p < 23) ? kPow10.v[p] : NAN; }

bool isSafeInteger(double x) {
  return std::isfinite(x) && std::floor(x) == x && std::fabs(x) <= static_cast<double>(kMaxSafeInteger);
}

bool parseFixed(const char* text, size_t length, int p, int64_t* out) {
  // Integer arithmetic on the digits, exact while every partial value is a safe integer (as TS's
  // doubles are; a true value above the limit fails the same check there).
  size_t i = 0;
  bool negative = false;
  if (length > 0 && (text[0] == '-' || text[0] == '+')) {
    negative = text[0] == '-';
    i++;
  }
  int64_t v = 0;
  int frac = -1;  // fraction digits seen; -1 before the point
  for (; i < length; i++) {
    const char c = text[i];
    if (c == '.') {
      frac = 0;
      continue;
    }
    v = v * 10 + (c - '0');
    if (v > kMaxSafeInteger) return false;
    if (frac >= 0 && ++frac > p) return false;
  }
  for (int k = frac < 0 ? 0 : frac; k < p; k++) {
    v *= 10;
    if (v > kMaxSafeInteger) return false;
  }
  *out = (negative && v != 0) ? -v : v;
  return true;
}

double roundHalfAwayFromZero(double x) {
  // Math.sign(x) * Math.floor(Math.abs(x) + 0.5); r === 0 ? 0 : r
  if (std::isnan(x)) return x;
  const double sign = x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0;
  const double r = sign * std::floor(std::fabs(x) + 0.5);
  return r == 0 ? 0.0 : r;
}

double jsRound(double x) {
  double r = std::floor(x);
  if (x - r >= 0.5) r += 1;
  if (r == 0) r = std::copysign(0.0, x);
  return r;
}

double toFixed(double b, int p) { return roundHalfAwayFromZero(b * pow10(p)); }

double parseDecimal(const char* text, size_t length) {
  // The exact fast path: at most 15 significant digits form an exact integer m, and with at most
  // 22 fraction digits m / 10^k is one correctly rounded division, as Number(text) is.
  size_t i = 0;
  bool negative = false;
  if (length > 0 && (text[0] == '-' || text[0] == '+')) {
    negative = text[0] == '-';
    i++;
  }
  int64_t m = 0;
  int significant = 0, fraction = 0;
  bool point = false, fast = true;
  for (; i < length; i++) {
    const char c = text[i];
    if (c == '.') {
      point = true;
      continue;
    }
    if (point) fraction++;
    if (m == 0 && c == '0') continue;  // leading zeros aren't significant
    if (++significant > 15) {
      fast = false;
      break;
    }
    m = m * 10 + (c - '0');
  }
  if (fast && fraction <= 22) {
    const double v = static_cast<double>(m) / kPow10.v[fraction];
    return negative ? -v : v;
  }
  // Past the fast path: strtod, correctly rounded in glibc, newlib and MSVC, in the "C" locale.
  // The text is passed as "0.<digits>e<exponent>" with the leading zeros dropped, so text of any
  // length fits: past kKept significant digits, the rest only matters as "something nonzero",
  // kept as one last 1. Every halfway point between two doubles has at most 767 significant
  // digits, so the value stays on the same side of each one and rounds the same. 768 digits in
  // all is also as many as MSVC's strtod reads: it drops the rest without looking at them.
  constexpr size_t kKept = 767;
  char buf[kKept + 40];
  size_t n = 0;
  buf[n++] = negative ? '-' : '+';
  buf[n++] = '0';
  buf[n++] = '.';
  int64_t exponent = 0;  // the value is 0.<digits> × 10^exponent
  size_t digits = 0;
  bool sticky = false, seenPoint = false;
  for (size_t k = (length > 0 && (text[0] == '-' || text[0] == '+')) ? 1 : 0; k < length; k++) {
    const char c = text[k];
    if (c == '.') {
      seenPoint = true;
      continue;
    }
    if (digits == 0 && c == '0') {
      if (seenPoint) exponent--;  // 0.00ddd: each zero after the point moves the digits right
      continue;
    }
    if (!seenPoint) exponent++;
    if (digits < kKept) {
      buf[n++] = c;
      digits++;
    } else if (c != '0') {
      sticky = true;
    }
  }
  if (digits == 0) return negative ? -0.0 : 0.0;
  if (sticky) buf[n++] = '1';
  buf[n++] = 'e';
  if (exponent < 0) {
    buf[n++] = '-';
    exponent = -exponent;
  }
  char rev[24];
  int e = 0;
  do {
    rev[e++] = static_cast<char>('0' + exponent % 10);
    exponent /= 10;
  } while (exponent != 0);
  while (e > 0) buf[n++] = rev[--e];
  buf[n] = '\0';
  return std::strtod(buf, nullptr);
}

// ---- Number text ----------------------------------------------------------------------------

namespace {

/** Writes the decimal digits of a non-negative integer below 2^63; returns the count. */
int writeDigits(uint64_t v, char* tmp) {
  char rev[24];
  int n = 0;
  do {
    rev[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  for (int k = 0; k < n; k++) tmp[k] = rev[n - 1 - k];
  return n;
}

struct Out {
  char* p;
  size_t cap;
  size_t len = 0;
  bool ok = true;
  void put(char c) {
    if (len < cap) p[len++] = c;
    else ok = false;
  }
  void put(const char* s, int n) {
    for (int k = 0; k < n; k++) put(s[k]);
  }
  int result() const { return ok ? static_cast<int>(len) : -1; }
};

}  // namespace

int formatFixed(double n, int p, bool leadingZero, char* out, size_t capacity) {
  if (!isSafeInteger(n) || p < 0 || p > 15) return -1;
  const bool negative = n < 0;
  const uint64_t abs = static_cast<uint64_t>(negative ? -n : n);
  const uint64_t scale = static_cast<uint64_t>(kPow10.v[p]);
  const uint64_t q = abs / scale, fracValue = abs % scale;
  Out o{out, capacity};
  char digits[24];
  const int nInt = writeDigits(q, digits);
  if (fracValue == 0) {
    if (q != 0 && negative) o.put('-');
    o.put(digits, nInt);
    return o.result();
  }
  char frac[24];
  int nFrac = writeDigits(fracValue, frac);
  // Left-pad to p digits, then trim trailing zeros.
  char padded[24];
  int pad = p - nFrac;
  for (int k = 0; k < pad; k++) padded[k] = '0';
  std::memcpy(padded + pad, frac, static_cast<size_t>(nFrac));
  int end = p;
  while (end > 0 && padded[end - 1] == '0') end--;
  if (negative) o.put('-');
  if (!(q == 0 && !leadingZero)) o.put(digits, nInt);
  o.put('.');
  o.put(padded, end);
  return o.result();
}

int formatDecimal(double x, int digits, bool keepZeros, char* out, size_t capacity) {
  if (!std::isfinite(x)) return -1;
  int d = digits;
  double n = toFixed(x, d);
  while (!isSafeInteger(n) && d > 0) n = toFixed(x, --d);
  if (!isSafeInteger(n)) return -1;
  char text[48];
  const int len = formatFixed(n, d, true, text, sizeof(text));
  if (len < 0) return -1;
  Out o{out, capacity};
  o.put(text, len);
  if (keepZeros && d > 0) {
    const char* dot = static_cast<const char*>(std::memchr(text, '.', static_cast<size_t>(len)));
    const int have = dot ? static_cast<int>(text + len - dot - 1) : 0;
    if (!dot) o.put('.');
    for (int k = have; k < d; k++) o.put('0');
  }
  return o.result();
}

int formatSignificant(double x, int significant, char* out, size_t capacity) {
  if (!(x > 0) || !std::isfinite(x) || significant < 1 || significant > 15) return -1;
  const double top = kPow10.v[significant];
  if (x >= top) {
    for (int e = 1; e <= 15; e++) {
      const double n = roundHalfAwayFromZero(x / kPow10.v[e]);
      if (n < top) return formatFixed(n * kPow10.v[e], 0, true, out, capacity);
    }
    return -1;
  }
  const double low = kPow10.v[significant - 1];
  for (int k = 0; k <= 15; k++) {
    const double n = roundHalfAwayFromZero(x * kPow10.v[k]);
    if (n >= low) return formatFixed(n, k, true, out, capacity);
  }
  Out o{out, capacity};
  o.put('0');
  return o.result();
}

int formatCount(double n, char* out, size_t capacity) {
  if (!isSafeInteger(n)) return -1;
  char digits[24];
  const int len = writeDigits(static_cast<uint64_t>(n < 0 ? -n : n), digits);
  Out o{out, capacity};
  if (n < 0) o.put('-');
  for (int i = 0; i < len; i++) {
    if (i > 0 && (len - i) % 3 == 0) o.put(',');
    o.put(digits[i]);
  }
  return o.result();
}

}  // namespace gcode
