// The capped report (ops/types.ts makeReport), streaming: see report.h.
#include "report.h"

#include <cstring>

namespace gcode {
namespace {

const uint32_t CAP = kMaxReportedIssues;
const size_t INITIAL_TEXT = 8 * 1024;

}  // namespace

bool IssueCollector::init() {
  for (Entry*& l : lists_) l = arena_.array<Entry>(CAP);
  order_ = arena_.array<Entry*>(3 * CAP);
  text_ = arena_.array<char>(INITIAL_TEXT);
  textCap_ = INITIAL_TEXT;
  return !arena_.failed;
}

bool IssueCollector::compact(size_t need) {
  // The live reasons, in the order they lie in the store, moved down over the dead ones.
  uint32_t n = 0;
  size_t live = 0;
  for (int s = 0; s < 3; s++) {
    for (uint32_t k = 0; k < size_[s]; k++) {
      Entry* e = &lists_[s][k];
      live += e->issue.reason.length;
      uint32_t at = n++;
      while (at > 0 && order_[at - 1]->issue.reason.data > e->issue.reason.data) {
        order_[at] = order_[at - 1];
        at--;
      }
      order_[at] = e;
    }
  }
  char* into = text_;
  size_t cap = textCap_;
  if (live + need > textCap_ / 2) {
    // Mostly live: a bigger store (the old one is left behind; the store at most doubles).
    cap = textCap_ * 2;
    while (live + need > cap / 2) cap *= 2;
    into = arena_.array<char>(cap);
    if (!into) return false;
  }
  size_t at = 0;
  for (uint32_t k = 0; k < n; k++) {
    Str& r = order_[k]->issue.reason;
    std::memmove(into + at, r.data, r.length);
    r.data = into + at;
    at += r.length;
  }
  text_ = into;
  textCap_ = cap;
  textUsed_ = at;
  dead_ = 0;
  return true;
}

bool IssueCollector::store(const char* text, size_t n, Str& out) {
  if (textUsed_ + n > textCap_ && !compact(n)) return false;
  std::memcpy(text_ + textUsed_, text, n);
  out = Str{text_ + textUsed_, static_cast<uint32_t>(n)};
  textUsed_ += n;
  return true;
}

bool IssueCollector::add(uint8_t source, Severity severity, const char* ruleId, const char* reason, size_t reasonLength, uint32_t line,
                         uint64_t offset, uint32_t length, bool choices) {
  const int s = static_cast<int>(severity);
  found_[s]++;
  const uint32_t seq = seq_++;
  Entry* list = lists_[s];
  uint32_t& size = size_[s];
  // Its place: after every entry with a smaller or equal (line, source), since its sequence is
  // the largest so far.
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    const Entry& m = list[mid];
    const bool before = m.issue.line < line || (m.issue.line == line && m.source <= source);
    if (before) lo = mid + 1;
    else hi = mid;
  }
  if (lo >= CAP) return true;  // sorts after all 200 kept: dropped
  if (size == CAP) {
    release(list[CAP - 1]);
    size--;
  }
  Entry e{};
  e.issue.line = line;
  e.issue.offset = offset;
  e.issue.length = length;
  e.issue.severity = severity;
  e.issue.ruleId = ruleId;
  e.issue.hasChoices = choices;
  e.source = source;
  e.seq = seq;
  // Copied before the entry joins the list, so a compaction doesn't see it half made.
  if (!store(reason, reasonLength, e.issue.reason)) return false;
  for (uint32_t k = size; k > lo; k--) list[k] = list[k - 1];
  list[lo] = e;
  size++;
  return true;
}

bool IssueCollector::finish(Report& out) {
  Issue* issues = arena_.array<Issue>(CAP);
  if (!issues) return false;
  uint32_t n = 0;
  for (int s = 0; s < 3; s++) {
    for (uint32_t k = 0; k < size_[s] && n < CAP; k++) issues[n++] = lists_[s][k].issue;
  }
  const uint32_t total = found_[0] + found_[1] + found_[2];
  out.issues = issues;
  out.count = n;
  out.omitted = total - n;
  out.blocked = found_[0] > 0;
  out.warnings = found_[1] > 0;
  out.arenaUsed = arena_.used();
  return true;
}

}  // namespace gcode
