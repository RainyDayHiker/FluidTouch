// MemoryLineReader: lines split as parse/document.ts splitLines does.
#include "gcode/io.h"

namespace gcode {

bool MemoryLineReader::next(Line& out) {
  if (at_ >= size_) return false;
  const size_t start = at_;
  size_t i = start;
  while (i < size_ && data_[i] != '\n' && data_[i] != '\r') i++;
  out.data = data_ + start;
  out.length = static_cast<uint32_t>(i - start);
  out.index = index_++;
  out.offset = start;
  if (i == size_) {
    out.ending = Ending::None;
    at_ = i;
  } else if (data_[i] == '\n') {
    out.ending = Ending::LF;
    at_ = i + 1;
  } else if (i + 1 < size_ && data_[i + 1] == '\n') {
    out.ending = Ending::CRLF;
    at_ = i + 2;
  } else {
    out.ending = Ending::CR;
    at_ = i + 1;
  }
  return true;
}

}  // namespace gcode
