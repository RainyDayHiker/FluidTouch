// Working memory from the caller's Arena (specs/15 §4.2): a bump allocator, and arrays that grow
// by moving to a larger block (the old one is abandoned; growth doubles, so at most half is lost).
// Running out sets `failed`, which the caller turns into OutOfMemory. The Outline Check also takes
// memory from the arena's top (15 §18.3), which it gives back when the bottom needs it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gcode/io.h"

namespace gcode {

/**
 * Gives back what it holds at the arena's top when the bottom needs it (15 §18.2: the analysis
 * always wins memory). Called at most once per registration.
 */
class ArenaYield {
 public:
  virtual void yieldTop() = 0;

 protected:
  ~ArenaYield() = default;
};

class ArenaAlloc {
 public:
  explicit ArenaAlloc(Arena a) : base_(static_cast<char*>(a.base)), size_(a.size) {}

  /**
   * From the bottom. When it doesn't fit and a holder of the top is registered, the holder gives
   * the top back first; past that, sets `failed`.
   */
  void* alloc(size_t bytes, size_t align = alignof(double)) {
    if (failed) return nullptr;
    void* p = tryAlloc(bytes, align);
    while (!p && yield_ && top_ > 0) {
      ArenaYield* y = yield_;
      yield_ = nullptr;
      y->yieldTop();
      p = tryAlloc(bytes, align);
    }
    if (!p) failed = true;
    return p;
  }

  /** From the bottom, or null without setting `failed` or asking the top to yield. */
  void* tryAlloc(size_t bytes, size_t align = alignof(double)) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(base_) + used_;
    const size_t pad = (align - p % align) % align;
    if (pad + bytes > size_ - top_ - used_) return nullptr;
    used_ += pad + bytes;
    note();
    return base_ + used_ - bytes;
  }

  /**
   * The free space at the bottom, aligned, without taking it: a scratch whose size isn't known
   * yet is written there, then taken with tryAlloc() once it is (the peak counts only that).
   */
  void* freeStart(size_t align, size_t& bytes) const {
    const uintptr_t p = reinterpret_cast<uintptr_t>(base_) + used_;
    const size_t pad = (align - p % align) % align;
    const size_t free = size_ - top_ - used_;
    bytes = pad < free ? free - pad : 0;
    return base_ + used_ + pad;
  }

  /** From the top, below what the top already holds; null when it doesn't fit (never sets `failed`). */
  void* allocTop(size_t bytes, size_t align = alignof(double)) {
    const uintptr_t lo = reinterpret_cast<uintptr_t>(base_) + used_;
    const uintptr_t end = reinterpret_cast<uintptr_t>(base_) + size_ - top_;
    if (bytes > end - lo) return nullptr;
    const uintptr_t start = (end - bytes) / align * align;
    if (start < lo) return nullptr;
    top_ = reinterpret_cast<uintptr_t>(base_) + size_ - start;
    note();
    return reinterpret_cast<void*>(start);
  }

  template <typename T>
  T* array(size_t n) {
    return static_cast<T*>(alloc(n * sizeof(T) + (n == 0 ? 1 : 0), alignof(T)));
  }

  /** A copy of `n` bytes, as a Str (empty on failure). */
  Str copy(const char* s, size_t n) {
    char* p = static_cast<char*>(alloc(n == 0 ? 1 : n, 1));
    if (!p) return Str{"", 0};
    std::memcpy(p, s, n);
    return Str{p, static_cast<uint32_t>(n)};
  }

  size_t used() const { return used_; }
  /** Back to an earlier used(): what was allocated since is free again. */
  void setUsed(size_t used) { used_ = used; }
  /** Bytes held at the top. */
  size_t top() const { return top_; }
  /** Back to an earlier top(): what was taken from the top since is free again. */
  void setTop(size_t top) { top_ = top; }
  /** Free bytes between the bottom and the top. */
  size_t free() const { return size_ - top_ - used_; }
  /** The most of the arena in use at once, bottom and top together. */
  size_t peak() const { return peak_; }
  char* base() const { return base_; }
  size_t size() const { return size_; }
  /** Registers (or, with null, clears) the holder of the top. */
  void setYield(ArenaYield* y) { yield_ = y; }

  bool failed = false;

 private:
  void note() {
    if (used_ + top_ > peak_) peak_ = used_ + top_;
  }
  char* base_;
  size_t size_;
  size_t used_ = 0;
  size_t top_ = 0;
  size_t peak_ = 0;
  ArenaYield* yield_ = nullptr;
};

/** A growable array in the arena, for trivially copyable T. */
template <typename T>
struct Vec {
  T* data = nullptr;
  uint32_t size = 0;
  uint32_t cap = 0;

  bool push(ArenaAlloc& a, const T& v) {
    if (size == cap) {
      const uint32_t next = cap ? cap * 2 : 8;
      T* grown = a.array<T>(next);
      if (!grown) return false;
      if (size) std::memcpy(static_cast<void*>(grown), data, size * sizeof(T));
      data = grown;
      cap = next;
    }
    data[size++] = v;
    return true;
  }
  T& back() { return data[size - 1]; }
};

/**
 * Builds a string piece by piece in a scratch buffer, then the caller copies it into the arena.
 * The scratch is sized for the longest text a call builds (two names of the longest line).
 */
struct TextBuilder {
  char* buf;
  size_t cap;
  size_t len = 0;
  TextBuilder(char* buf, size_t cap) : buf(buf), cap(cap) {}
  void put(const char* s, size_t n) {
    for (size_t k = 0; k < n && len < cap; k++) buf[len++] = s[k];
  }
  void put(const char* s) { put(s, std::strlen(s)); }
  void put(Str s) { put(s.data, s.length); }
};

}  // namespace gcode
