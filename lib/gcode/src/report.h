// A transform's report, built as issues arrive (specs/15 §5.3 "The cap"). Internal to the library.
//
// TS joins every issue its checks find, sorts them stably by severity and line, and keeps the
// first 200 (makeReport). Streaming, issues arrive out of line order (W5 is found late against an
// early line, the through-cut warnings at the end), so each severity keeps its own list of at most
// 200, sorted by (line, source, sequence): source is the check (0 classification, 1 file-level,
// 2 Flip's, 3 overflow), sequence the order the issue was found in. That is the order TS's stable
// sort leaves them in. The report is then the lists in severity order, cut at 200 in all.
//
// Reasons are copied into a text store that compacts when it fills, so the issues the cap drops
// don't use up the arena on a file with many issues.
#pragma once

#include "arena.h"
#include "gcode/transform.h"

namespace gcode {

class IssueCollector {
 public:
  explicit IssueCollector(ArenaAlloc& arena) : arena_(arena) {}

  /** Allocates the lists; false when out of memory. */
  bool init();

  /** One issue. `line` is 1-based; `offset` and `length` locate its text. False when out of memory. */
  bool add(uint8_t source, Severity severity, const char* ruleId, const char* reason, size_t reasonLength, uint32_t line, uint64_t offset,
           uint32_t length, bool choices);

  /** The report (makeReport); its issues live in the arena. False when out of memory. */
  bool finish(Report& out);

  /** Issues found of a severity, kept or not. */
  uint32_t found(Severity s) const { return found_[static_cast<int>(s)]; }

 private:
  struct Entry {
    Issue issue;
    uint8_t source;
    uint32_t seq;
  };
  bool store(const char* text, size_t n, Str& out);
  void release(const Entry& e) { dead_ += e.issue.reason.length; }
  bool compact(size_t need);

  ArenaAlloc& arena_;
  Entry* lists_[3] = {nullptr, nullptr, nullptr};
  uint32_t size_[3] = {0, 0, 0};
  uint32_t found_[3] = {0, 0, 0};
  uint32_t seq_ = 0;
  char* text_ = nullptr;
  size_t textCap_ = 0, textUsed_ = 0, dead_ = 0;
  Entry** order_ = nullptr;  // scratch for compaction
};

}  // namespace gcode
