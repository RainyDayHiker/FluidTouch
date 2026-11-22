// SHA-256 (FIPS 180-4) for the tests (specs/15 §9.4): an output against the TS bytes' digest,
// without storing it. No platform code and no standard library containers: the device runner
// uses it as the host tests do.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace gcode_test {

class Sha256 {
 public:
  Sha256() {
    static const uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, kInit, sizeof h_);
  }
  void update(const char* p, size_t n) {
    bits_ += static_cast<uint64_t>(n) * 8;
    while (n > 0) {
      const size_t k = n < 64 - used_ ? n : 64 - used_;
      std::memcpy(buf_ + used_, p, k);
      used_ += k;
      p += k;
      n -= k;
      if (used_ == 64) {
        block(buf_);
        used_ = 0;
      }
    }
  }
  /** The digest as 64 lowercase hex digits (ends the hash). */
  void hex(char out[65]) {
    const uint64_t bits = bits_;
    const char pad = static_cast<char>(0x80), zero = 0;
    update(&pad, 1);
    while (used_ != 56) update(&zero, 1);
    char len[8];
    for (int k = 0; k < 8; k++) len[k] = static_cast<char>(bits >> (56 - 8 * k));
    update(len, 8);
    static const char d[] = "0123456789abcdef";
    for (int k = 0; k < 32; k++) {
      const unsigned b = (h_[k / 4] >> (24 - 8 * (k % 4))) & 0xffu;
      out[2 * k] = d[b >> 4];
      out[2 * k + 1] = d[b & 15];
    }
    out[64] = '\0';
  }

 private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const unsigned char* b) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
        0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
        0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
        0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int t = 0; t < 16; t++) {
      w[t] = static_cast<uint32_t>(b[4 * t]) << 24 | static_cast<uint32_t>(b[4 * t + 1]) << 16 | static_cast<uint32_t>(b[4 * t + 2]) << 8 |
             static_cast<uint32_t>(b[4 * t + 3]);
    }
    for (int t = 16; t < 64; t++) {
      const uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
      const uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
      w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h_[0], bb = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int t = 0; t < 64; t++) {
      const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + w[t];
      const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = bb;
      bb = a;
      a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += bb;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
  }
  uint32_t h_[8];
  unsigned char buf_[64];
  size_t used_ = 0;
  uint64_t bits_ = 0;
};

}  // namespace gcode_test
