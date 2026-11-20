// Working memory from the caller's Arena (specs/15 §4.2): a bump allocator, and arrays that grow
// by moving to a larger block (the old one is abandoned; growth doubles, so at most half is lost).
// Nothing is freed; running out sets `failed`, which the caller turns into OutOfMemory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gcode/io.h"

namespace gcode {

class ArenaAlloc {
 public:
  explicit ArenaAlloc(Arena a) : base_(static_cast<char*>(a.base)), size_(a.size) {}

  void* alloc(size_t bytes, size_t align = alignof(double)) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(base_) + used_;
    const size_t pad = (align - p % align) % align;
    if (failed || pad + bytes > size_ - used_) {
      failed = true;
      return nullptr;
    }
    used_ += pad + bytes;
    return base_ + used_ - bytes;
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
  bool failed = false;

 private:
  char* base_;
  size_t size_;
  size_t used_ = 0;
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
