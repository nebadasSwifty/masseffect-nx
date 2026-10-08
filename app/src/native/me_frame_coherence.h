// Frame coherence of ring draws (masseffect_native_coherence_stats, measurement only).
//
// Question it answers: how many draws of a frame are identical, in everything the per-draw work of the ring thread
// depends on, to a draw of the previous frame? If most are, that work (pipeline lookup, descriptor and texture
// bindings, constant packing, index conversion, vertex copies) could be reused instead of recomputed
// (docs/frame-coherence.md).
//
// Every ring draw of a "recording" frame gets a set of 64-bit keys, one per component:
//   shaders    the paired VS/PS library entries and the hashes of the loaded microcode
//   state      render state registers (surfaces, depth/stencil, blend, raster, viewport, scissor, AA, bool/loop
//              constants)
//   textures   the fetch constant words of the texture slots the VS/PS samplers use
//   constants  the float constants the VS/PS read (constants_bytes of each shader)
//   geometry   draw initiator, index DMA registers, plus the address/size/order of every index and vertex range
//              the Vulkan draw fingerprints, and those fingerprints
//   fetch words all 192 fetch constant words (conservative: unused stale slots count too)
//   content    only the index/vertex fingerprints (address independent)
//   full       shaders + state + textures + constants + geometry
//   strict     full + fetch words
// The vertex/index fingerprints are not recomputed: the Vulkan draw reports the ones it already computes
// (NoteContent). A range it did not fingerprint (over the dedupe size limit, dedupe off) is hashed here by sampling
// (SampleHash, counted in the report); a range without data makes the draw's geometry, full and strict keys unique
// (never matched).
//
// Frames are recorded in pairs: with every = N, frames f with f % N == 0 and f % N == 1 are recorded and the second
// is compared with the first (N <= 1: every frame, each compared with the previous one). Comparison is by the same
// ordinal (draw i against draw i of the previous frame) and at any position (hash set of the previous frame's keys).
// The ring time of each compared draw (from before PairDraw to the end of Draw, CNTPCT) and the C6 stage times of
// the timed draws (1 in 64, NoteStage) are split between draws whose full key matched and the others.
//
// Only the ring thread calls this. Off: Recording() is one load and a branch; NoteContent / NoteStage are one load
// and a branch; nothing is allocated.
#pragma once

#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "me_ring_partition.h"  // Now(), TicksPerSecond(): CNTPCT_EL0 on the Switch

namespace me::native::coherence {

enum Key : uint32_t {
  kShaders = 0,
  kState,
  kTextures,
  kConstants,
  kGeometry,
  kFetchAll,
  kContent,
  kFull,
  kStrict,
  kKeys
};
inline constexpr uint32_t kParts = 5;  // kShaders..kGeometry make up kFull
inline constexpr const char* kKeyNames[kKeys] = {"shaders",     "state",   "textures", "constants", "geometry",
                                                 "fetch words", "content", "full",     "strict"};
inline constexpr uint32_t kStages = 12;  // DrawsVulkanImpl stages_ns_ (0-6 partition a recorded draw)
inline constexpr const char* kStageNames[7] = {"state",   "indices",  "textures", "upload space and pass",
                                               "uploads", "pipeline and constants", "recording"};
inline constexpr uint32_t kSetBits = 13;
inline constexpr uint32_t kSetSize = 1u << kSetBits;  // 8192 slots, at most kMaxDraws keys: load <= 0.5
inline constexpr uint32_t kMaxDraws = 4096;           // recorded draws per frame; more are counted as overflow

// Render state words (first register, count). Register numbers from rex/graphics/register_table.inc.
inline constexpr uint16_t kStateRanges[][2] = {
    {0x2000, 6},     // RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, RB_COLOR1..3_INFO
    {0x200E, 2},     // PA_SC_SCREEN_SCISSOR_TL/BR
    {0x2080, 3},     // PA_SC_WINDOW_OFFSET, PA_SC_WINDOW_SCISSOR_TL/BR
    {0x2103, 0x12},  // VGT_MULTI_PRIM_IB_RESET_INDX, RB_COLOR_MASK, RB_BLEND_*, RB_STENCILREFMASK(_BF),
                     // RB_ALPHA_REF, PA_CL_VPORT_*
    {0x2180, 2},     // SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC
    {0x2200, 12},    // RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL, PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL,
                     // PA_CL_VTE_CNTL, RB_MODECONTROL, RB_BLENDCONTROL1..3
    {0x2280, 3},     // PA_SU_POINT_SIZE, PA_SU_POINT_MINMAX, PA_SU_LINE_CNTL
    {0x2301, 6},     // PA_SC_AA_CONFIG, PA_SU_VTX_CNTL, PA_CL_GB_*
    {0x2312, 1},     // PA_SC_AA_MASK
    {0x2380, 4},     // PA_SU_POLY_OFFSET_*
    {0x4900, 0x28},  // SHADER_CONSTANT_BOOL_*, SHADER_CONSTANT_LOOP_*
};
inline constexpr uint32_t kStateWords = 6 + 2 + 3 + 0x12 + 2 + 12 + 3 + 6 + 1 + 4 + 0x28;
inline constexpr uint32_t kRegFetch = 0x4800;        // SHADER_CONSTANT_FETCH_00_0, 32 slots x 6 words
inline constexpr uint32_t kRegConstantsVs = 0x4000;  // SHADER_CONSTANT_000_X
inline constexpr uint32_t kRegConstantsPs = 0x4400;  // SHADER_CONSTANT_256_X
inline constexpr uint32_t kRegMinRequired = 0x4928;  // the register file must hold at least this many words

inline uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xBF58476D1CE4E5B9ull;
}

// Open-addressing set of nonzero 64-bit keys (0 is mapped to 1), cleared per frame.
struct KeySet {
  std::vector<uint64_t> slots;
  uint32_t count = 0;
  void Init() { slots.assign(kSetSize, 0); count = 0; }
  void Clear() {
    if (count) std::fill(slots.begin(), slots.end(), 0);
    count = 0;
  }
  static uint32_t Home(uint64_t k) { return uint32_t((k * 0x9E3779B97F4A7C15ull) >> (64 - kSetBits)); }
  // true if the key was not in the set
  bool Insert(uint64_t k) {
    k = k ? k : 1;
    for (uint32_t i = Home(k);; i = (i + 1) & (kSetSize - 1)) {
      if (slots[i] == k) return false;
      if (!slots[i]) {
        slots[i] = k;
        ++count;
        return true;
      }
    }
  }
  bool Contains(uint64_t k) const {
    k = k ? k : 1;
    for (uint32_t i = Home(k);; i = (i + 1) & (kSetSize - 1)) {
      if (slots[i] == k) return true;
      if (!slots[i]) return false;
    }
  }
};

struct DrawKeys {
  std::array<uint64_t, kKeys> k{};
};

// What the ring knows about the draw when it ends (NativeGraphicsSystem).
struct Inputs {
  const uint32_t* registers = nullptr;  // register file, at least kRegMinRequired words
  const void* vs = nullptr;             // paired library entries
  const void* ps = nullptr;
  uint64_t vs_hash = 0, ps_hash = 0;    // hashes of the loaded microcode
  uint32_t vs_constant_words = 0, ps_constant_words = 0;  // float constant words each shader reads
  uint64_t generation_constants_vs = 0, generation_constants_ps = 0;  // ring sink generations (0 = unknown)
  uint32_t initiator = 0, dma_base = 0, dma_size = 0;
  const uint8_t* texture_slots = nullptr;  // fetch constant slots of the samplers (0-31)
  uint32_t texture_slot_count = 0;
};

struct Window {
  uint64_t frames = 0, draws = 0, prev_draws = 0, repeats = 0, more = 0;
  std::array<uint64_t, kKeys> any{}, ordinal{}, distinct{};
  std::array<uint64_t, kParts> only_one{};
  uint64_t content_draws = 0, unfingerprinted_draws = 0, sampled_ranges = 0, dma_draws = 0, dma_without_index = 0;
  uint64_t ticks_all = 0, ticks_matched = 0, draws_matched = 0;
  std::array<std::array<uint64_t, kStages>, 2> stage_ns{};
  std::array<uint64_t, 2> timed{};
  uint64_t recorded = 0, overflow = 0, diag_ticks = 0, frames_all = 0, frames_recorded = 0;
};

struct State {
  bool on = false;
  uint32_t every = 8;
  uint64_t frame = 0;
  bool recording = false, comparing = false, prev_valid = false;
  // current draw
  bool in_draw = false, timed = false, unfingerprinted = false, index_noted = false;
  uint64_t sampled = 0;  // ranges hashed by SampleHash in the current draw
  uint64_t t0 = 0, geometry = 0, content = 0;
  uint32_t notes = 0;
  std::array<uint64_t, kStages> stage_ns{};
  uint64_t frame_repeats = 0, unique = 0;
  std::vector<DrawKeys> cur, prev;
  std::array<KeySet, kKeys> set_cur, set_prev;
  // constants memo: the hash of a stage's constants while its generation and size do not change
  uint64_t memo_gen_vs = 0, memo_gen_ps = 0, memo_hash_vs = 0, memo_hash_ps = 0;
  uint32_t memo_words_vs = UINT32_MAX, memo_words_ps = UINT32_MAX;
  Window w;
  uint64_t window_start = 0, next_report = 0;
};

inline State g;  // ring thread only

inline bool Recording() { return g.recording; }

// Ring thread start. every: frame pair period (<= 1 = every frame).
inline void Configure(bool on, uint32_t every) {
  g = State{};
  g.on = on;
  if (!on) return;
  g.every = every;
  g.cur.reserve(kMaxDraws);
  g.prev.reserve(kMaxDraws);
  for (uint32_t k = 0; k < kKeys; ++k) {
    g.set_cur[k].Init();
    g.set_prev[k].Init();
  }
  g.recording = true;  // frame 0 records
  g.window_start = ring_partition::Now();
  g.next_report = g.window_start + uint64_t(10.0 * ring_partition::TicksPerSecond());
}

// Before PairDraw of a ring draw on a recording frame.
inline void BeginDraw() {
  g.in_draw = true;
  g.timed = false;
  g.unfingerprinted = false;
  g.index_noted = false;
  g.sampled = 0;
  g.geometry = 0;
  g.content = 0;
  g.notes = 0;
  g.t0 = ring_partition::Now();
}

// Sampled hash of a range the Vulkan draw did not fingerprint (over its size limit): the first and the last 1 KB
// and 8 evenly spaced 1 KB blocks in between (at most 10 KB read). Only on recording frames. A change outside the
// sampled blocks is missed, so for these ranges the match share errs on the high side; the report counts them.
inline uint64_t SampleHash(const uint8_t* data, uint64_t bytes) {
  constexpr uint64_t kBlock = 1024, kInner = 8;
  if (bytes <= (kInner + 2) * kBlock) return XXH3_64bits(data, size_t(bytes));
  uint64_t h = XXH3_64bits_withSeed(data, kBlock, bytes);
  const uint64_t step = (bytes - kBlock) / (kInner + 1);
  for (uint64_t i = 1; i <= kInner; ++i) h = XXH3_64bits_withSeed(data + i * step, kBlock, h);
  return XXH3_64bits_withSeed(data + bytes - kBlock, kBlock, h);
}

// Vulkan draw: an index (index = true) or vertex range and the fingerprint it computed for it (0 = none; the range
// is then hashed here by sampling, if data is given, and counted).
inline void NoteContent(uint64_t address, uint64_t bytes_order, uint64_t fingerprint, const uint8_t* data,
                        uint64_t bytes, bool index) {
  if (!g.in_draw) return;
  ++g.notes;
  if (index) g.index_noted = true;
  g.geometry = Mix(Mix(g.geometry, address), bytes_order);
  if (!fingerprint) {
    if (data && bytes) {
      fingerprint = SampleHash(data, bytes) | 1;
      ++g.sampled;
    } else {
      g.unfingerprinted = true;
    }
  }
  g.content = Mix(g.content, fingerprint);
}

// Vulkan draw: C6 stage stopwatch (timed draws only).
inline void NoteStage(size_t stage, uint64_t ns) {
  if (!g.in_draw || stage >= kStages) return;
  g.timed = true;
  g.stage_ns[stage] += ns;
}

inline uint64_t HashConstants(const uint32_t* regs, uint32_t base, uint32_t words, uint64_t generation,
                              uint64_t& memo_gen, uint32_t& memo_words, uint64_t& memo_hash) {
  if (generation && generation == memo_gen && words == memo_words) return memo_hash;
  words = std::min<uint32_t>(words, 1024);
  memo_hash = XXH3_64bits_withSeed(regs + base, size_t(words) * 4, words);
  memo_gen = generation;
  memo_words = words;
  return memo_hash;
}

inline DrawKeys ComputeKeys(const Inputs& in) {
  DrawKeys d;
  const uint32_t* r = in.registers;
  d.k[kShaders] = Mix(Mix(Mix(Mix(0, uint64_t(reinterpret_cast<uintptr_t>(in.vs))),
                              uint64_t(reinterpret_cast<uintptr_t>(in.ps))),
                          in.vs_hash),
                      in.ps_hash);
  uint32_t words[kStateWords];
  uint32_t n = 0;
  for (const auto& range : kStateRanges) {
    std::memcpy(words + n, r + range[0], size_t(range[1]) * 4);
    n += range[1];
  }
  d.k[kState] = XXH3_64bits(words, sizeof(words));
  uint64_t textures = in.texture_slot_count;
  for (uint32_t i = 0; i < in.texture_slot_count; ++i) {
    const uint32_t slot = in.texture_slots[i] & 31;
    textures = Mix(textures, XXH3_64bits_withSeed(r + kRegFetch + slot * 6, 24, slot));
  }
  d.k[kTextures] = textures;
  const uint64_t cvs = HashConstants(r, kRegConstantsVs, in.vs_constant_words, in.generation_constants_vs,
                                     g.memo_gen_vs, g.memo_words_vs, g.memo_hash_vs);
  const uint64_t cps = HashConstants(r, kRegConstantsPs, in.ps_constant_words, in.generation_constants_ps,
                                     g.memo_gen_ps, g.memo_words_ps, g.memo_hash_ps);
  d.k[kConstants] = Mix(cvs, cps);
  const uint32_t vgt[3] = {r[0x2100], r[0x2101], r[0x2102]};  // VGT_MAX/MIN_VTX_INDX, VGT_INDX_OFFSET
  d.k[kGeometry] = Mix(Mix(Mix(Mix(g.geometry, in.initiator), uint64_t(in.dma_base) << 32 | in.dma_size),
                           XXH3_64bits(vgt, sizeof(vgt))),
                       g.content);
  d.k[kFetchAll] = XXH3_64bits(r + kRegFetch, 192 * 4);
  d.k[kContent] = Mix(g.content, g.notes);
  if (g.unfingerprinted) {  // content unknown: never matched
    const uint64_t unique = 0xD6E8FEB86659FD93ull ^ ++g.unique;
    d.k[kGeometry] = unique;
    d.k[kContent] = unique;
  }
  uint64_t full = 0;
  for (uint32_t k = 0; k < kParts; ++k) full = Mix(full, d.k[k]);
  d.k[kFull] = full;
  d.k[kStrict] = Mix(full, d.k[kFetchAll]);
  return d;
}

// After the draw (also for draws that were not identified or not recorded by Vulkan).
inline void EndDraw(const Inputs& in) {
  const uint64_t t1 = ring_partition::Now();
  if (!g.in_draw) return;
  g.in_draw = false;
  Window& w = g.w;
  ++w.recorded;
  if (g.cur.size() >= kMaxDraws || !in.registers) {
    ++w.overflow;
    g.stage_ns.fill(0);
    return;
  }
  const DrawKeys d = ComputeKeys(in);
  const size_t ordinal = g.cur.size();
  g.cur.push_back(d);
  for (uint32_t k = 0; k < kKeys; ++k) {
    const bool fresh = g.set_cur[k].Insert(d.k[k]);
    if (k == kFull && !fresh) ++g.frame_repeats;
  }
  if (g.comparing) {
    ++w.draws;
    if (g.notes) ++w.content_draws;
    if (g.unfingerprinted) ++w.unfingerprinted_draws;
    w.sampled_ranges += g.sampled;
    if (((in.initiator >> 6) & 3) == 0) {  // DMA indices
      ++w.dma_draws;
      if (!g.index_noted) ++w.dma_without_index;
    }
    for (uint32_t k = 0; k < kKeys; ++k) {
      if (g.set_prev[k].Contains(d.k[k])) ++w.any[k];
      if (ordinal < g.prev.size() && g.prev[ordinal].k[k] == d.k[k]) ++w.ordinal[k];
    }
    if (ordinal < g.prev.size() && g.prev[ordinal].k[kFull] != d.k[kFull]) {
      uint32_t differ = 0, which = 0;
      for (uint32_t k = 0; k < kParts; ++k)
        if (g.prev[ordinal].k[k] != d.k[k]) {
          ++differ;
          which = k;
        }
      if (differ == 1)
        ++w.only_one[which];
      else
        ++w.more;
    }
    const bool matched = g.set_prev[kFull].Contains(d.k[kFull]);
    const uint64_t ticks = t1 - g.t0;
    w.ticks_all += ticks;
    if (matched) {
      w.ticks_matched += ticks;
      ++w.draws_matched;
    }
    if (g.timed) {
      for (uint32_t s = 0; s < kStages; ++s) w.stage_ns[matched][s] += g.stage_ns[s];
      ++w.timed[matched];
    }
  }
  if (g.timed) g.stage_ns.fill(0);
  w.diag_ticks += ring_partition::Now() - t1;
}

inline std::string Format(const char* f, double a = 0, double b = 0, double c = 0, double d = 0) {
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), f, a, b, c, d);
  return buffer;
}

// Builds the two report lines of the window that ends now and starts a new one.
inline void BuildReport(uint64_t now, std::string& line1, std::string& line2) {
  const Window& w = g.w;
  const double secs = double(now - g.window_start) / ring_partition::TicksPerSecond();
  const double draws = double(std::max<uint64_t>(1, w.draws));
  const double frames = double(std::max<uint64_t>(1, w.frames));
  auto pct = [&](uint64_t v) { return 100.0 * double(v) / draws; };
  line1 = Format("[native] frame coherence (%.1f s, every %.0f frames: %.0f frame pairs compared, ", secs,
                 double(g.every), double(w.frames)) +
          Format("%.0f draws, %.1f draws per frame, previous frames %.1f): ", double(w.draws), double(w.draws) / frames,
                 double(w.prev_draws) / frames) +
          Format("full key in the previous frame %.1f %% (same ordinal %.1f %%), strict %.1f %% (%.1f %%), ",
                 pct(w.any[kFull]), pct(w.ordinal[kFull]), pct(w.any[kStrict]), pct(w.ordinal[kStrict])) +
          Format("repeats within the frame %.1f %%, distinct full keys per frame %.1f", pct(w.repeats),
                 double(w.distinct[kFull]) / frames);
  for (uint32_t k = 0; k < kKeys; ++k) {
    if (k == kFull || k == kStrict) continue;
    line1 += std::string(" | ") + kKeyNames[k] +
             Format(" %.1f %% (ordinal %.1f %%), %.1f distinct per frame", pct(w.any[k]), pct(w.ordinal[k]),
                    double(w.distinct[k]) / frames);
  }
  line1 += Format(" | vertex/index fingerprints in %.1f %% of draws, %.1f %% with a range not fingerprinted, "
                  "%.2f ranges per draw hashed by sampling here",
                  pct(w.content_draws), pct(w.unfingerprinted_draws), double(w.sampled_ranges) / draws) +
           Format(", %.0f DMA-index draws of which %.0f without an index fingerprint (geometry by address only)",
                  double(w.dma_draws), double(w.dma_without_index));
  // Line 2: what differs, time, stages, estimate, cost.
  const double tick_us = 1e6 / ring_partition::TicksPerSecond();
  const double matched = double(std::max<uint64_t>(1, w.draws_matched));
  const double others = double(std::max<uint64_t>(1, w.draws - w.draws_matched));
  line2 = Format("[native] frame coherence: draws differing from the same ordinal in one component only: shaders "
                 "%.0f, state %.0f, textures %.0f, constants %.0f",
                 double(w.only_one[0]), double(w.only_one[1]), double(w.only_one[2]), double(w.only_one[3])) +
          Format(", geometry %.0f, in two or more %.0f", double(w.only_one[4]), double(w.more)) +
          Format(" | ring time per draw (PairDraw to end of Draw): all %.2f us, full-key matches %.2f us, others "
                 "%.2f us; matches are %.1f %% of the time",
                 double(w.ticks_all) * tick_us / draws, double(w.ticks_matched) * tick_us / matched,
                 double(w.ticks_all - w.ticks_matched) * tick_us / others,
                 w.ticks_all ? 100.0 * double(w.ticks_matched) / double(w.ticks_all) : 0.0);
  double reusable = 0;
  for (uint32_t m = 0; m < 2; ++m) {
    const double timed = double(std::max<uint64_t>(1, w.timed[1 - m]));
    line2 += m == 0 ? " | C6 stages of full-key matches (" : " | C6 stages of the others (";
    line2 += Format("%.0f timed draws, us per timed draw):", double(w.timed[1 - m]));
    double sum = 0;
    for (uint32_t s = 0; s < 7; ++s) {
      const double us = double(w.stage_ns[1 - m][s]) / 1e3 / timed;
      sum += us;
      line2 += std::string(" ") + kStageNames[s] + Format(" %.2f", us);
      if (m == 0 && (s == 0 || s == 1 || s == 2 || s == 5)) reusable += us;
    }
    line2 += Format(", sum %.2f", sum);
  }
  line2 += Format(" | estimate: stages state + indices + textures + pipeline and constants of the full-key matches "
                  "= %.2f us per compared draw",
                  reusable * double(w.draws_matched) / draws);
  line2 += Format(" | diagnostic cost %.2f us per recorded draw (%.0f recorded, %.0f over the per-frame cap",
                  w.recorded ? double(w.diag_ticks) * tick_us / double(w.recorded) : 0.0, double(w.recorded),
                  double(w.overflow)) +
           Format("; %.0f of %.0f frames recorded)", double(w.frames_recorded), double(w.frames_all));
  g.w = Window{};
  g.window_start = now;
  g.next_report = now + uint64_t(10.0 * ring_partition::TicksPerSecond());
}

// At every Swap. Returns true when the 10 s report is ready (two lines to log).
inline bool EndFrame(std::string& line1, std::string& line2) {
  if (!g.on) return false;
  Window& w = g.w;
  ++w.frames_all;
  if (g.recording) {
    ++w.frames_recorded;
    if (g.comparing) {
      ++w.frames;
      w.prev_draws += g.prev.size();
      w.repeats += g.frame_repeats;
      for (uint32_t k = 0; k < kKeys; ++k) w.distinct[k] += g.set_cur[k].count;
    }
    std::swap(g.cur, g.prev);
    std::swap(g.set_cur, g.set_prev);
  }
  g.prev_valid = g.recording;
  g.in_draw = false;
  g.frame_repeats = 0;
  ++g.frame;
  g.recording = g.every <= 1 || (g.frame % g.every) <= 1;
  g.comparing = g.recording && g.prev_valid;
  if (g.recording) {
    g.cur.clear();
    for (KeySet& set : g.set_cur) set.Clear();
  }
  const uint64_t now = ring_partition::Now();
  if (now < g.next_report) return false;
  BuildReport(now, line1, line2);
  return true;
}

}  // namespace me::native::coherence
