// Line flags and the tokenizer (specs/04 §4–5): packages/core/src/model/flags.ts and
// parse/tokenizer.ts. Positions are byte positions within the line (15 §4.3).
#pragma once

#include <cstddef>
#include <cstdint>

namespace gcode {

/** LineFlag bits (model/flags.ts): parse flags 0–11, simulation flags 12–18. */
enum LineFlag : uint32_t {
  kMalformedNumber = 1u << 0,
  kUnclosedComment = 1u << 1,
  kJogCommand = 1u << 2,
  kParametricSyntax = 1u << 3,
  kChecksum = 1u << 4,
  kUnrecognizedSyntax = 1u << 5,
  kUnknownLetter = 1u << 6,
  kUnknownGCode = 1u << 7,
  kUnknownMCode = 1u << 8,
  kDuplicateWord = 1u << 9,
  kPrecisionOverflow = 1u << 10,
  kModalGroupConflict = 1u << 11,
  kMotionModeAssumed = 1u << 12,
  kDistanceModeAssumed = 1u << 13,
  kUnitsAssumed = 1u << 14,
  kIncrementalFromUnknown = 1u << 15,
  kUnknownStart = 1u << 16,
  kArcInvalid = 1u << 17,
  kArcNotXY = 1u << 18,
};

/** A token's kind (model/types.ts). */
enum class TokenKind : uint8_t { Word, Comment, Space, Raw };
/** A comment's style; None for other tokens. */
enum class CommentStyle : uint8_t { None, Semicolon, Paren };

/** One token (model/types.ts Token). Tokens cover the line fully and in order. */
struct Token {
  TokenKind kind;  ///< Word, comment, space or raw.
  /** Words: the upper-cased letter. */
  char letter;
  CommentStyle style;  ///< Comments: `;` or `(`.
  /** Words: fraction digits of the number. */
  uint8_t fractionDigits;
  uint16_t start;   ///< Byte position in the line.
  uint16_t length;  ///< Bytes.
  /** Words: where the number's text begins (after blanks following the letter). */
  uint16_t valueStart;
};

/** Receives a line's tokens in order (parse/tokenizer.ts TokenSink). */
class TokenSink {
 public:
  /** A word [start, end): its upper-cased letter, where its number starts, and its fraction digits. */
  virtual void word(uint32_t start, uint32_t end, char letter, uint32_t valueStart, int fractionDigits) = 0;
  /** Blanks (spaces and tabs) [start, end). */
  virtual void space(uint32_t start, uint32_t end) = 0;
  /** A comment [start, end), `;` to the end of the line or `(` to the next `)`. */
  virtual void comment(uint32_t start, uint32_t end, CommentStyle style) = 0;
  /** Anything else [start, end): `$`/`%` lines and unrecognized spans (with a flag on the line). */
  virtual void raw(uint32_t start, uint32_t end) = 0;

 protected:
  ~TokenSink() = default;
};

/** scanLine: reports each token of the line to `sink`; returns the tokenizer's flags. */
uint32_t scanLine(const char* text, size_t length, TokenSink& sink);

/** parseLineFlags: the tokenizer's flags plus the vocabulary and structural flags (04 §5). */
uint32_t parseLineFlags(const char* text, size_t length);

/**
 * A G or M word's value as a code key (parse/vocabulary.ts canonicalCode): integer part × 10
 * plus one fraction digit, after dropping leading and trailing zeros (`G01` → 10, `G28.10` →
 * 281). -1 for a signed value (no code) and -2 for one no recognized code can have (a longer
 * fraction, or an integer part above 999).
 */
int codeKey(const char* text, size_t length);

}  // namespace gcode
