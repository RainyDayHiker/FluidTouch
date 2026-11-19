// The tokenizer: packages/core/src/parse/tokenizer.ts. Reads bytes; only ASCII is interpreted,
// so a byte at or above 0x80 behaves as a non-ASCII UTF-16 unit does in TS (15 §4.3).
#include <cstring>

#include "gcode/tokens.h"

namespace gcode {
namespace {

const int TAB = 9, SPACE = 32, PLUS = 43, MINUS = 45, DOT = 46;

struct Text {
  const unsigned char* p;
  size_t n;
  /** charCodeAt: -1 past the end (TS's NaN, which every comparison rejects). */
  int at(size_t i) const { return i < n ? p[i] : -1; }
};

bool isBlank(int c) { return c == SPACE || c == TAB; }
bool isDigit(int c) { return c >= 48 && c <= 57; }
bool isLetter(int c) { return (c >= 65 && c <= 90) || (c >= 97 && c <= 122); }
/** Characters that start some other token kind: `;(#[]*.+-`. */
bool startsToken(int c) {
  return c == ';' || c == '(' || c == '#' || c == '[' || c == ']' || c == '*' || c == '.' || c == '+' || c == '-';
}

size_t skipBlanks(const Text& t, size_t i) {
  while (i < t.n && isBlank(t.p[i])) i++;
  return i;
}

size_t skipDigits(const Text& t, size_t i) {
  while (i < t.n && isDigit(t.p[i])) i++;
  return i;
}

/** scanNumber: sets end / fraction and returns true for a number at i. */
bool scanNumber(const Text& t, size_t i, size_t& end, int& fraction) {
  size_t j = i;
  const int c = t.at(j);
  if (c == PLUS || c == MINUS) j++;
  const size_t intEnd = skipDigits(t, j);
  size_t digits = intEnd - j;
  size_t e = intEnd;
  int frac = 0;
  if (t.at(intEnd) == DOT) {
    const size_t fracEnd = skipDigits(t, intEnd + 1);
    frac = static_cast<int>(fracEnd - (intEnd + 1));
    digits += static_cast<size_t>(frac);
    e = fracEnd;
  }
  if (digits == 0) return false;
  end = e;
  fraction = frac;
  return true;
}

}  // namespace

uint32_t scanLine(const char* text, size_t length, TokenSink& sink) {
  const Text t{reinterpret_cast<const unsigned char*>(text), length};
  uint32_t flags = 0;
  const size_t n = length;
  const auto u = [](size_t v) { return static_cast<uint32_t>(v); };

  // `$` and `%` lines are one raw token; `$J=` is a jog command.
  const size_t first = skipBlanks(t, 0);
  const int lead = t.at(first);
  if (lead == '$' || lead == '%') {
    if (lead == '$' && (t.at(first + 1) == 'J' || t.at(first + 1) == 'j') && t.at(first + 2) == '=') flags |= kJogCommand;
    if (n > 0) sink.raw(0, u(n));
    return flags;
  }

  bool lastWasWord = false;
  const auto raw = [&](size_t start, size_t end, uint32_t flag) {
    sink.raw(u(start), u(end));
    flags |= flag;
    lastWasWord = false;
    return end;
  };

  size_t i = 0, numEnd = 0;
  int numFraction = 0;
  while (i < n) {
    const int c = t.p[i];
    if (isBlank(c)) {
      const size_t end = skipBlanks(t, i);
      sink.space(u(i), u(end));
      i = end;
    } else if (c == ';') {
      sink.comment(u(i), u(n), CommentStyle::Semicolon);
      lastWasWord = false;
      i = n;
    } else if (c == '(') {
      const void* close = i + 1 < n ? std::memchr(t.p + i + 1, ')', n - i - 1) : nullptr;
      const size_t end = close ? static_cast<size_t>(static_cast<const unsigned char*>(close) - t.p) + 1 : n;
      if (!close) flags |= kUnclosedComment;
      sink.comment(u(i), u(end), CommentStyle::Paren);
      lastWasWord = false;
      i = end;
    } else if (isLetter(c)) {
      const int upper = c >= 97 ? c - 32 : c;
      const size_t k = skipBlanks(t, i + 1);
      const bool isNum = scanNumber(t, k, numEnd, numFraction);
      if (upper == 'O') {
        i = raw(i, isNum ? numEnd : i + 1, kParametricSyntax);
      } else if (isNum) {
        sink.word(u(i), u(numEnd), static_cast<char>(upper), u(k), numFraction);
        lastWasWord = true;
        i = numEnd;
      } else {
        const int next = t.at(k);
        if ((next == PLUS || next == MINUS) && scanNumber(t, skipBlanks(t, k + 1), numEnd, numFraction)) {
          i = raw(i, numEnd, kMalformedNumber);
        } else {
          i = raw(i, i + 1, next == '#' || next == '[' ? kParametricSyntax : kUnrecognizedSyntax);
        }
      }
    } else if (c == '#') {
      i = raw(i, skipDigits(t, i + 1), kParametricSyntax);
    } else if (c == '[' || c == ']') {
      i = raw(i, i + 1, kParametricSyntax);
    } else if (c == '*') {
      i = raw(i, skipDigits(t, i + 1), kChecksum);
    } else if ((isDigit(c) || c == DOT || c == PLUS || c == MINUS) && scanNumber(t, i, numEnd, numFraction)) {
      i = raw(i, numEnd, lastWasWord ? kMalformedNumber : kUnrecognizedSyntax);
    } else {
      size_t end = i + 1;
      while (end < n) {
        const int d = t.p[end];
        if (isBlank(d) || isLetter(d) || isDigit(d) || startsToken(d)) break;
        end++;
      }
      i = raw(i, end, kUnrecognizedSyntax);
    }
  }
  return flags;
}

}  // namespace gcode
