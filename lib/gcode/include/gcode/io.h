// Lines in, bytes out, and working memory (specs/15 §4.2, §5). The library reads a file through
// a LineReader the caller implements (an SD file, memory, a std file), writes through a
// ByteWriter, and takes all its working memory from a caller-supplied Arena.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gcode {

/** How a line ended in the file. */
enum class Ending : uint8_t { None, LF, CRLF, CR };

/** One line of the file, without its ending. */
struct Line {
  /** The line's bytes; valid until the next next() or rewind(). */
  const char* data;
  uint32_t length;  ///< Bytes of data.
  Ending ending;    ///< How the line ended in the file.
  /** 0-based. */
  uint32_t index;
  /** Byte offset of the line's first byte in the file. */
  uint64_t offset;
};

/**
 * Supplies the file's lines in order (implemented by the caller). Lines are split exactly as
 * the TS core's splitLines does: `\r\n`, `\n` or `\r` ends a line, a final line may have no
 * ending, and a file ending with a newline has no empty line after it. MemoryLineReader does
 * this for bytes in memory; a file reader splits its chunks the same way.
 */
class LineReader {
 public:
  /** The next line; false at the end of the file or on a read error. */
  virtual bool next(Line& out) = 0;
  /** Back to the first line, for another pass. */
  virtual bool rewind() = 0;
  /** Whether next() returned false because of a read error rather than the end. */
  virtual bool failed() const = 0;

 protected:
  ~LineReader() = default;
};

/** A LineReader over bytes already in memory (tests, or a file the caller holds whole). */
class MemoryLineReader final : public LineReader {
 public:
  /** Reads `size` bytes at `data`, which must outlive the reader. */
  MemoryLineReader(const char* data, size_t size) : data_(data), size_(size) {}
  bool next(Line& out) override;
  bool rewind() override {
    at_ = 0;
    index_ = 0;
    return true;
  }
  bool failed() const override { return false; }

 private:
  const char* data_;
  size_t size_;
  size_t at_ = 0;
  uint32_t index_ = 0;
};

/** Receives an output file's bytes (implemented by the caller). */
class ByteWriter {
 public:
  /** Writes `length` bytes; false on a write error. */
  virtual bool write(const char* data, size_t length) = 0;

 protected:
  ~ByteWriter() = default;
};

/** Working memory: a block the caller owns, used by one call at a time (15 §4.2). */
struct Arena {
  void* base;   ///< The block's first byte.
  size_t size;  ///< Its size in bytes.
};

/** A byte string inside the arena or the caller's memory; not terminated. */
struct Str {
  const char* data;  ///< The bytes.
  uint32_t length;   ///< How many.
};

/** Outcome of a call (15 §5.2). */
enum class Status : uint8_t {
  Ok,
  /** A UTF-16/32 BOM or a NUL byte (04 §2); see Analysis::refusedMessage. */
  Refused,
  /** A line longer than the reader limit (15 §4.2); see Analysis::longLine. */
  LineTooLong,
  ReadError,
  OutOfMemory,
  Cancelled,
  /** A transform's report has a Blocking issue, so nothing was written (15 §5.3). */
  Blocked,
  /** A choice the library doesn't support yet: Flip's "Flip and reverse cuts" until C4. */
  NotSupported,
  /** The ByteWriter failed. */
  WriteError,
  /** applyChecked(): the file doesn't have the lines and bytes its check() read. */
  FileChanged,
};

/**
 * Called every CHECKPOINT_LINES (10,000) lines with the line index and its byte offset.
 * Returning false cancels the call (15 §4.5).
 */
using Progress = bool (*)(void* context, uint32_t line, uint64_t offset);

/** How often (in lines) Progress is called: CHECKPOINT_LINES. */
constexpr uint32_t kCheckpointLines = 10000;

}  // namespace gcode
