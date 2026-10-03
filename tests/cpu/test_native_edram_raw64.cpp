#include "me_edram_layout.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>

// Integer-only shader transcription: never convert half encodings to float.
using HalfPixel = std::array<uint16_t, 4>;
constexpr uint32_t Word(const HalfPixel& p, unsigned half) {
  return uint32_t(p[half * 2]) | (uint32_t(p[half * 2 + 1]) << 16);
}
constexpr HalfPixel Pixel(uint32_t lo, uint32_t hi) {
  return {uint16_t(lo), uint16_t(lo >> 16), uint16_t(hi), uint16_t(hi >> 16)};
}
struct Coordinate { uint32_t x, y, half; };
Coordinate ShaderCoordinate(unsigned tile, unsigned word, unsigned row,
                            unsigned pitch, unsigned msaa, bool wide,
                            bool depth) {
  const me::native::EdramLayout l(msaa, wide);
  if (depth) word = word < 40 ? word + 40 : word - 40;
  return {((tile % pitch) * (80u >> unsigned(wide)) +
           (word >> unsigned(wide))) >> l.x_log2,
          ((tile / pitch) * 16 + row) >> l.y_log2, wide ? word & 1u : 0u};
}
// Independent division-based physical-word oracle, including replicated
// guest samples in the current single-sample host-image approximation.
Coordinate ReferenceCoordinate(unsigned tile, unsigned word, unsigned row,
                               unsigned pitch, unsigned msaa, bool wide,
                               bool depth) {
  const unsigned words_per_sample = wide ? 2 : 1;
  const unsigned samples_x = msaa == 2 ? 2 : 1;
  const unsigned samples_y = msaa ? 2 : 1;
  const unsigned adjusted = depth ? (word + 40) % 80 : word;
  const unsigned physical_x = (tile % pitch) * 80 + adjusted;
  return {physical_x / words_per_sample / samples_x,
          ((tile / pitch) * 16 + row) / samples_y,
          wide ? physical_x % 2 : 0u};
}
uint32_t Payload(Coordinate p, bool wide) {
  // Includes signed zeros, infinities, signalling/quiet NaNs and payloads.
  constexpr HalfPixel special[] = {
      {0x0000, 0x8000, 0x7C00, 0xFC00},
      {0x7C01, 0x7E01, 0xFE55, 0xFFFF},
      {0x0001, 0x03FF, 0x0400, 0x7BFF}};
  if (wide) return Word(special[(p.x + p.y) % 3], p.half);
  return 0x9E3779B9u * (p.x + 1) ^ (0xA5A55A5Au + p.y * 0x1020304u);
}
int main() {
  for (unsigned h = 0; h != 65536; ++h) {
    // Every encoding in every lane; neighbouring lanes are unequal.
    const HalfPixel p = {uint16_t(h), uint16_t(h ^ 0x8000),
                         uint16_t(h ^ 0x7FFF), uint16_t(h ^ 0xFFFF)};
    assert(Pixel(Word(p, 0), Word(p, 1)) == p);
    const HalfPixel q = {p[2], p[0], p[3], p[1]};
    assert(Pixel(Word(q, 0), Word(q, 1)) == q);
  }
  uint64_t words_checked = 0, writes_checked = 0;
  for (unsigned sm = 0; sm != 3; ++sm) {
    for (unsigned dm = 0; dm != 3; ++dm) {
      for (bool sw : {false, true}) for (bool dw : {false, true}) {
        for (bool sd : {false, true}) for (bool dd : {false, true}) {
          if ((sd && sw) || (dd && dw)) continue; // Depth is always 32 bpp.
          // Different pitches, offsets and rows cross both image tile rows.
          constexpr unsigned sp = 7, dp = 13, start_s = 3, start_d = 5, count = 19;
          const me::native::EdramLayout dest(dm, dw);
          std::map<std::pair<unsigned, unsigned>, unsigned> writer_count;
          for (unsigned t = 0; t != count; ++t) for (unsigned row = 0; row != 16; ++row) {
            for (unsigned w = 0; w != 80; ++w) {
              const auto s = ShaderCoordinate(start_s + t, w, row, sp, sm, sw, sd);
              const auto r = ReferenceCoordinate(start_s + t, w, row, sp, sm, sw, sd);
              assert(s.x == r.x && s.y == r.y && s.half == r.half);
              assert(Payload(s, sw) == Payload(r, sw));
              ++words_checked;
              // Compute transfer invocation's single-writer filter.
              if (w % (1u << (unsigned(dw) + dest.x_log2)) ||
                  row % (1u << dest.y_log2)) continue;
              const auto d = ShaderCoordinate(start_d + t, w, row, dp, dm, dw, dd);
              const auto dr = ReferenceCoordinate(start_d + t, w, row, dp, dm, dw, dd);
              assert(d.x == dr.x && d.y == dr.y && d.half == dr.half);
              assert((++writer_count[{d.x, d.y}] == 1));
              if (dw) {
                assert(w + 1 < 80); // A 64-bit texel never straddles tiles.
                const auto hi = ShaderCoordinate(start_s + t, w + 1, row, sp, sm, sw, sd);
                const auto hr = ReferenceCoordinate(start_s + t, w + 1, row, sp, sm, sw, sd);
                const uint32_t lo_word = Payload(s, sw), hi_word = Payload(hi, sw);
                const auto packed = Pixel(lo_word, hi_word);
                assert(Word(packed, 0) == Payload(r, sw));
                assert(Word(packed, 1) == Payload(hr, sw));
              }
              ++writes_checked;
            }
          }
        }
      }
    }
  }
  // Depth/color 40-word permutation is involutive and preserves word parity;
  // same-class raw32/raw64 transfer must NOT apply it.
  for (unsigned w = 0; w != 80; ++w) {
    const unsigned swapped = (w + 40) % 80;
    assert((swapped + 40) % 80 == w);
    assert((swapped & 1) == (w & 1));
  }
  std::printf("raw64 EDRAM: all 65536 half encodings, %llu physical words, "
              "%llu unique writes; all 9 MSAA pairs and mixed 32/64 layouts passed\n",
              static_cast<unsigned long long>(words_checked),
              static_cast<unsigned long long>(writes_checked));
}
