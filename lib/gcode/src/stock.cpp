// Stock comments: `(Stock: XMIN=… XMAX=… YMIN=… YMAX=… ZMIN=… ZMAX=…)` (ops/stock.ts). The
// regexes there are emulated exactly: PARAM is a global search, DECIMAL a full match.
#include "stock.h"

#include <cstring>

#include "gcode/num.h"

namespace gcode {
namespace {

const char* const KEYS[6] = {"XMIN", "XMAX", "YMIN", "YMAX", "ZMIN", "ZMAX"};

bool isBlank(char c) { return c == ' ' || c == '\t'; }
bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isWord(char c) { return isAlpha(c) || isDigit(c) || c == '_'; }
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; }

/** DECIMAL: /^[+-]?(\d+\.?\d*|\.\d+)$/ */
bool isDecimal(const char* s, size_t n) {
  size_t i = 0;
  if (i < n && (s[i] == '+' || s[i] == '-')) i++;
  if (i < n && isDigit(s[i])) {
    while (i < n && isDigit(s[i])) i++;
    if (i < n && s[i] == '.') i++;
    while (i < n && isDigit(s[i])) i++;
    return i == n;
  }
  if (i < n && s[i] == '.') {
    i++;
    if (i >= n || !isDigit(s[i])) return false;
    while (i < n && isDigit(s[i])) i++;
    return i == n;
  }
  return false;
}

}  // namespace

namespace {

/** One comment body [bodyStart, bodyEnd): a stock comment when it starts with "Stock:". */
bool readBody(const char* text, size_t bodyStart, size_t bodyEnd, uint32_t line, StockVisitor& out) {
  size_t at = bodyStart;
  const size_t end = bodyEnd;
  while (at < end && isBlank(text[at])) at++;
  // /^Stock[ \t]*:/i
  static const char kStock[] = "stock";
  size_t k = 0;
  while (k < 5 && at + k < end && (text[at + k] | 0x20) == kStock[k]) k++;
  if (k < 5) return true;
  size_t p = at + 5;
  while (p < end && isBlank(text[p])) p++;
  if (p >= end || text[p] != ':') return true;
  const size_t paramsAt = p + 1;

  StockComment c{};
  c.line = line;
  bool have[6] = {false, false, false, false, false, false};
  // PARAM, global: /([A-Za-z][A-Za-z0-9_]*)[ \t]*=[ \t]*([^ \t,)]+)/g over [paramsAt, end).
  size_t s = paramsAt;
  while (s < end) {
    if (!isAlpha(text[s])) {
      s++;
      continue;
    }
    size_t nameEnd = s + 1;
    while (nameEnd < end && isWord(text[nameEnd])) nameEnd++;
    size_t q = nameEnd;
    while (q < end && isBlank(text[q])) q++;
    if (q >= end || text[q] != '=') {
      s++;
      continue;
    }
    q++;
    while (q < end && isBlank(text[q])) q++;
    size_t valueEnd = q;
    while (valueEnd < end && !isBlank(text[valueEnd]) && text[valueEnd] != ',' && text[valueEnd] != ')') valueEnd++;
    if (valueEnd == q) {
      s++;
      continue;
    }
    // A match: [s, valueEnd), name [s, nameEnd), value [q, valueEnd).
    int key = -1;
    if (nameEnd - s == 4) {
      for (int kk = 0; kk < 6; kk++) {
        bool same = true;
        for (int ch = 0; ch < 4; ch++) same = same && upper(text[s + ch]) == KEYS[kk][ch];
        if (same) key = kk;
      }
    }
    if (key >= 0 && !have[key] && isDecimal(text + q, valueEnd - q)) {
      const char* dot = static_cast<const char*>(std::memchr(text + q, '.', valueEnd - q));
      StockValue& v = c.values[key];
      v.start = static_cast<uint16_t>(q);
      v.end = static_cast<uint16_t>(valueEnd);
      v.fractionDigits = static_cast<uint8_t>(dot ? valueEnd - (dot - text) - 1 : 0);
      v.text = Str{text + q, static_cast<uint32_t>(valueEnd - q)};
      have[key] = true;
    }
    s = valueEnd;
  }
  if (!(have[0] && have[1] && have[2] && have[3] && have[4] && have[5])) return true;
  const auto n = [&](int kk) { return parseDecimal(c.values[kk].text.data, c.values[kk].text.length); };
  if (n(1) > n(0) && n(3) > n(2) && n(5) > n(4)) {
    if (!out.comment(c)) return false;
  }
  return true;
}

}  // namespace

bool eachStockComment(const char* text, size_t length, uint32_t line, StockVisitor& out) {
  // Comment bodies: `( … )` (to the next `)`) and `; …` (to the end of the line), each read as
  // soon as it's found.
  for (size_t i = 0; i < length; i++) {
    size_t bodyStart, bodyEnd;
    bool last = false;
    if (text[i] == '(') {
      const void* close = i + 1 < length ? std::memchr(text + i + 1, ')', length - i - 1) : nullptr;
      bodyStart = i + 1;
      bodyEnd = close ? static_cast<size_t>(static_cast<const char*>(close) - text) : length;
      i = bodyEnd;
    } else if (text[i] == ';') {
      bodyStart = i + 1;
      bodyEnd = length;
      last = true;
    } else {
      continue;
    }
    if (!readBody(text, bodyStart, bodyEnd, line, out)) return false;
    if (last) break;
  }
  return true;
}

bool stockCommentsIn(const char* text, size_t length, uint32_t line, ArenaAlloc& arena, Vec<StockComment>& out) {
  // The value texts are copied: the line's bytes are valid only until the next read.
  class Keep final : public StockVisitor {
   public:
    Keep(ArenaAlloc& a, Vec<StockComment>& o) : arena(a), out(o) {}
    ArenaAlloc& arena;
    Vec<StockComment>& out;
    bool comment(const StockComment& c) override {
      StockComment kept = c;
      for (StockValue& v : kept.values) v.text = arena.copy(v.text.data, v.text.length);
      return out.push(arena, kept) && !arena.failed;
    }
  } keep(arena, out);
  return eachStockComment(text, length, line, keep) && !arena.failed;
}

bool stockBox(const StockComment& c, int p, Extents& box) {
  int64_t v[4];
  for (int k = 0; k < 4; k++) {
    if (!parseFixed(c.values[k].text.data, c.values[k].text.length, p, &v[k])) return false;
  }
  box.present = true;
  box.minX = static_cast<double>(v[0]);
  box.maxX = static_cast<double>(v[1]);
  box.minY = static_cast<double>(v[2]);
  box.maxY = static_cast<double>(v[3]);
  return true;
}

}  // namespace gcode
