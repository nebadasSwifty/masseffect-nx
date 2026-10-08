// Host test of app/src/native/me_shader_load_memo.h (masseffect_native_load_memo): a Find() hit must imply that the
// raw guest words are equal to the stored load, hence the same swapped microcode and the same XXH3 (the identity
// memo key). Random programs at random addresses, rewrites of single words (D3D patches vertex fetches in place),
// size changes, stage changes, slot collisions and reloads, against a reference map of the last load per
// (stage, address, size).
//   clang++ -std=c++20 -O2 -I app/src/native -I sdk/thirdparty/xxHash tests/cpu/test_native_shader_load_memo.cpp \
//     -o /tmp/test_native_shader_load_memo && /tmp/test_native_shader_load_memo
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <tuple>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "me_shader_load_memo.h"

using me::native::ShaderLoadMemo;

static int g_failures = 0;
#define CHECK(c)                                               \
  do {                                                         \
    if (!(c)) {                                                \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);         \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

static uint32_t Swap(uint32_t v) { return __builtin_bswap32(v); }

int main() {
  std::mt19937_64 rng(11);
  auto memo = std::make_unique<ShaderLoadMemo>();
  // Guest memory: 64 shader programs at fixed addresses, some sharing an address with different sizes.
  struct Program { uint32_t address, type; std::vector<uint32_t> raw; };
  std::vector<Program> programs;
  for (int i = 0; i < 64; ++i) {
    Program p;
    p.address = 0x40000000u + uint32_t(rng() % 2048) * 0x100u;
    p.type = uint32_t(rng() & 1);
    p.raw.resize(1 + rng() % (i % 7 == 0 ? 1200 : 300));
    for (auto& w : p.raw) w = uint32_t(rng());
    programs.push_back(std::move(p));
  }
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::vector<uint32_t>> last;  // reference: last stored raw
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint64_t> generation_of;
  uint64_t g_generation = 0;
  uint64_t hits = 0, misses = 0, rewrites = 0;
  for (int iteration = 0; iteration < 400000; ++iteration) {
    Program& p = programs[rng() % programs.size()];
    if (rng() % 50 == 0) {  // the game patches a word in place (vertex fetch rewrite)
      p.raw[rng() % p.raw.size()] ^= uint32_t(1) << (rng() % 32);
      ++rewrites;
    }
    const uint32_t words = uint32_t(p.raw.size());
    const auto key = std::make_tuple(p.type, p.address, words);
    const ShaderLoadMemo::Slot* slot = memo->Find(p.type, p.address, p.raw.data(), words);
    if (slot) {
      ++hits;
      // A hit: the reference must hold exactly these raw words, and the slot's swapped words and hash must be
      // what the full path would compute now.
      auto it = last.find(key);
      CHECK(it != last.end() && it->second == p.raw);
      CHECK(slot->swapped.size() == words);
      bool equal = true;
      for (uint32_t i = 0; i < words; ++i) equal &= slot->swapped[i] == Swap(p.raw[i]);
      CHECK(equal);
      CHECK(slot->memo_hash == XXH3_64bits(slot->swapped.data(), words * 4));
      // The generation restored on a hit is the one given when exactly these words were stored.
      CHECK(slot->generation == generation_of[key]);
    } else {
      ++misses;
      std::vector<uint32_t> swapped(words);
      for (uint32_t i = 0; i < words; ++i) swapped[i] = Swap(p.raw[i]);
      const uint64_t generation = ++g_generation;
      memo->Store(p.type, p.address, swapped, XXH3_64bits(swapped.data(), words * 4), generation);
      last[key] = p.raw;
      generation_of[key] = generation;
    }
  }
  // Address 0 (immediate loads) and oversize programs are never stored.
  std::vector<uint32_t> v(8, 1);
  memo->Store(0, 0, v, 1);
  CHECK(memo->Find(0, 0, v.data(), 8) == nullptr);
  std::vector<uint32_t> big(ShaderLoadMemo::kMaxWords + 1, 2);
  memo->Store(0, 0x41000000u, big, 1);
  CHECK(memo->Find(0, 0x41000000u, big.data(), uint32_t(big.size())) == nullptr);
  // Same address and size, other stage: no hit.
  std::vector<uint32_t> w(16, 0x12345678u);
  memo->Store(0, 0x42000000u, w, 5);
  std::vector<uint32_t> raw_w(16, Swap(0x12345678u));
  CHECK(memo->Find(1, 0x42000000u, raw_w.data(), 16) == nullptr);
  CHECK(memo->Find(0, 0x42000000u, raw_w.data(), 16) != nullptr);
  memo->Clear();
  CHECK(memo->Find(0, 0x42000000u, raw_w.data(), 16) == nullptr);
  std::printf("shader load memo: %llu hits, %llu misses, %llu rewrites; %s\n", (unsigned long long)hits,
              (unsigned long long)misses, (unsigned long long)rewrites, g_failures ? "FAILED" : "OK");
  return g_failures ? 1 : 0;
}
