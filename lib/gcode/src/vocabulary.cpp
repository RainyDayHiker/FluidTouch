// Recognized vocabulary and parse-time flags: packages/core/src/parse/vocabulary.ts.
#include "vocabulary.h"

#include "gcode/tokens.h"

namespace gcode {

int codeKey(const char* text, size_t length) {
  if (length > 0 && (text[0] == '-' || text[0] == '+')) return -1;
  size_t dot = length;
  for (size_t i = 0; i < length; i++) {
    if (text[i] == '.') {
      dot = i;
      break;
    }
  }
  // Integer part without leading zeros (one digit kept); fraction without trailing zeros.
  size_t intStart = 0;
  while (intStart + 1 < dot && text[intStart] == '0') intStart++;
  size_t fracEnd = length;
  if (dot < length) {
    while (fracEnd > dot + 1 && text[fracEnd - 1] == '0') fracEnd--;
  }
  const size_t intLen = dot - intStart, fracLen = dot < length ? fracEnd - dot - 1 : 0;
  if (intLen > 3 || fracLen > 1) return -2;
  int v = 0;
  for (size_t i = intStart; i < dot; i++) v = v * 10 + (text[i] - '0');
  return v * 10 + (fracLen ? text[dot + 1] - '0' : 0);
}

namespace vocab {
namespace {

// RECOGNIZED_G in its order (simulator.ts G_CODE_ORDER), as code keys.
const int kGKeys[] = {
    0, 10, 20, 30, 40, 100, 170, 180, 190, 200, 210, 280, 281, 300, 301, 382, 383, 384, 385, 400, 410, 420, 430, 431,
    490, 530, 540, 550, 560, 570, 580, 590, 610, 611, 640, 730, 760, 800, 810, 820, 830, 840, 850, 860, 870, 880, 890,
    900, 901, 910, 911, 920, 921, 922, 923, 930, 940, 950,
};
// RECOGNIZED_M in its order (M_CODE_ORDER).
const int kMKeys[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 300, 560, 620, 630, 640, 650, 670, 680};

// Grbl's modal groups (vocabulary.ts `groups`, in order: the bit is the group's position).
const int kMotion[] = {0, 10, 20, 30, 382, 383, 384, 385, 730, 760, 800, 810, 820, 830, 840, 850, 860, 870, 880, 890};
const int kPlane[] = {170, 180, 190};
const int kDistance[] = {900, 910};
const int kArcDistance[] = {901, 911};
const int kFeed[] = {930, 940, 950};
const int kUnits[] = {200, 210};
const int kCutterComp[] = {400, 410, 420};
const int kToolLength[] = {430, 431, 490};
const int kWcs[] = {540, 550, 560, 570, 580, 590};
const int kPath[] = {610, 611, 640};
const int kNonModal[] = {40, 100, 280, 281, 300, 301, 530, 920, 921, 922, 923};

template <size_t N>
int indexIn(const int (&keys)[N], int key) {
  for (size_t i = 0; i < N; i++) {
    if (keys[i] == key) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

static_assert(sizeof(kGKeys) / sizeof(int) <= 64, "G masks are two 32-bit words");

int gIndex(int key) { return key < 0 ? -1 : indexIn(kGKeys, key); }
int mIndex(int key) { return key < 0 ? -1 : indexIn(kMKeys, key); }

uint32_t groupBit(int key) {
  if (key < 0) return 0;
  if (indexIn(kMotion, key) >= 0) return 1u << 0;
  if (indexIn(kPlane, key) >= 0) return 1u << 1;
  if (indexIn(kDistance, key) >= 0) return 1u << 2;
  if (indexIn(kArcDistance, key) >= 0) return 1u << 3;
  if (indexIn(kFeed, key) >= 0) return 1u << 4;
  if (indexIn(kUnits, key) >= 0) return 1u << 5;
  if (indexIn(kCutterComp, key) >= 0) return 1u << 6;
  if (indexIn(kToolLength, key) >= 0) return 1u << 7;
  if (indexIn(kWcs, key) >= 0) return 1u << 8;
  if (indexIn(kPath, key) >= 0) return 1u << 9;
  if (indexIn(kNonModal, key) >= 0) return 1u << 10;
  return 0;
}

bool letterRecognized(char letter) {
  // EFGHIJKLMNPQRSTXYZABCUVW: every letter but D and O.
  return letter >= 'A' && letter <= 'Z' && letter != 'D' && letter != 'O';
}

}  // namespace vocab

namespace {

/** vocabulary.ts VocabularyCheck over one line's words. */
class VocabularySink final : public TokenSink {
 public:
  explicit VocabularySink(const char* text) : text_(text) {}
  uint32_t flags = 0;

  void word(uint32_t, uint32_t end, char letter, uint32_t valueStart, int) override {
    if (!vocab::letterRecognized(letter)) {
      flags |= kUnknownLetter;
    } else if (letter == 'G') {
      const uint32_t group = vocab::groupBit(codeKey(text_ + valueStart, end - valueStart));
      if (group == 0) {
        flags |= kUnknownGCode;
        return;
      }
      if (groups_ & group) flags |= kModalGroupConflict;
      groups_ |= group;
    } else if (letter == 'M') {
      if (vocab::mIndex(codeKey(text_ + valueStart, end - valueStart)) < 0) flags |= kUnknownMCode;
    } else {
      const uint32_t bit = 1u << (letter - 'A');
      if (letters_ & bit) flags |= kDuplicateWord;
      letters_ |= bit;
    }
  }
  void space(uint32_t, uint32_t) override {}
  void comment(uint32_t, uint32_t, CommentStyle) override {}
  void raw(uint32_t, uint32_t) override {}

 private:
  const char* text_;
  uint32_t letters_ = 0;
  uint32_t groups_ = 0;
};

}  // namespace

uint32_t parseLineFlags(const char* text, size_t length) {
  VocabularySink sink(text);
  return scanLine(text, length, sink) | sink.flags;
}

}  // namespace gcode
