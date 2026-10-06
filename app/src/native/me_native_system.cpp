// The native graphics system: an IGraphicsSystem that the app installs in config.graphics, so the Xenos emulation
// plugin is not loaded and no guest memory is watched. The game keeps its own Direct3D; a ring thread reads the PM4
// command list it writes, keeps a copy of the GPU registers, returns the read pointer, delivers interrupts, writes
// SCRATCH_REG/MEM_WRITE/EVENT_WRITE values the game waits for, and counts draws, copies and Swaps. Draws, resolves
// and Swaps are handed to the modules in src/native/masseffect (TargetsNative, DrawsVulkan, ShadersNative)
// with the native shader package (masseffect_shaders.mesp next to the executable).
//
// The app uses it when masseffect_renderer_native is true (see masseffect_app.h).

#include "me_native_system.h"
#include "me_shader_identity.h"
#include "me_shader_candidate_policy.h"
#include "me_native_draw_extent_estimator.h"
#include "me_pm4_runs.h"
#include "me_object_table.h"
#include "me_record_table.h"
#include "me_native_ps_no_kill.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/cvar.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <rex/sys_counters.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>

#include "masseffect/masseffect_deferred_recording.h"
#include "masseffect/masseffect_native_targets.h"
#include "masseffect/masseffect_native_shaders.h"
#include "masseffect/masseffect_shader_library.h"

namespace masseffect::native {
extern const ShadersNative* g_active_library;  // me_masseffect_glue.cpp
}

REXCVAR_DECLARE(bool, masseffect_vblank_sleep_exact);  // me_audio_hooks.cpp

// Read by MassEffectApp::PatchCoalescedHashForLocalTesting (masseffect_app.h). The Switch has no environment
// variables: debug runs set it in masseffect.toml.
REXCVAR_DEFINE_STRING(masseffect_coalesced_sha1, "", "Mass Effect",
                      "Local testing: SHA-1 (40 hex digits) of a modified Coalesced.ini (tools/coalesced.py, e.g. "
                      "no-startup-movies) accepted instead of the disc's; the same as MASSEFFECT_COALESCED_SHA1")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_shaders_index, true, "Mass Effect",
                    "If masseffect_shaders.mesp.idx matches the package, load only that index (entry table and "
                    "original containers) and read each shader's SPIR-V from the package on first use; otherwise, or "
                    "with false, load the whole package as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_repeated_identity, true, "Mass Effect",
                    "Native renderer: a shader IM_LOAD identical (same address and microcode) to the stage's current "
                    "program keeps its identification instead of hashing and matching again. false = always")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_several_rect, true, "Mass Effect",
                    "Native renderer: auto-index rect lists of 2..8 rectangles (D3D Clear of several regions) are "
                    "proven per rectangle, so mode 4 skips the transfers they overwrite. false = only single rects")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_renderer_native, true, "Mass Effect",
                    "Switch: use the native renderer (true) or ReXGlue's Xenos emulation (false, needs "
                    "gpu_plugin = \"xenos\")")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_extent_packed_formats, false, "Mass Effect",
                    "Draw extent proofs also accept packed vertex position formats (16_16, 16_16_FLOAT, 16_16_16_16(_FLOAT), "
                    "8_8_8_8, 2_10_10_10), decoded by the SDK interpreter: full-screen quads written with them become "
                    "proven overwrites and skip the EDRAM conversion of the old content. false = 32-bit floats only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_stencil_inert, true, "Mass Effect",
                    "EDRAM mode 4: a draw whose stencil test is ALWAYS and writes nothing counts as stencil-off "
                    "(full depth overwrite proof; lazy stencil kept where it is)");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_center_ogl, true, "Mass Effect",
                    "EDRAM mode 4 proofs: honor OpenGL pixel centers (PA_SU_VTX_CNTL.pix_center) instead of always "
                    "assuming D3D centers (which lost the first column/row of full-screen passes)");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_ps_no_kill, true, "Mass Effect",
                    "EDRAM mode 4 proofs: a pixel shader with no KILL anywhere (fetches and branches allowed) cannot "
                    "discard, not only straight-line ALU shaders");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_vs_alu, true, "Mass Effect",
                    "EDRAM mode 4 proofs: the rectangle estimator admits vertex shaders that compute the position "
                    "with ALU math and (non-relative) constants, run on the live constant bank");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_quad_triangles, true, "Mass Effect",
                    "EDRAM mode 4 proofs: a full-screen quad drawn as two triangles (6-vertex list / 4-vertex strip) "
                    "that are exactly an axis-aligned rectangle split on a diagonal is proven like a rectangle");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_clip_inside, true, "Mass Effect",
                    "EDRAM mode 4 proofs: a rectangle drawn with clipping on (no user planes) and every vertex inside "
                    "the clip volume, with an XY viewport transform, is proven like an unclipped screen-space one");
REXCVAR_DEFINE_BOOL(masseffect_native_registers_table, true, "Mass Effect",
                    "Draw records (game thread -> ring) in a fixed bucket table instead of a std::map: no "
                    "allocation per draw under the shared lock")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_fast_registers, true, "Mass Effect",
                    "Ring: runs of register writes into the constant/fetch range skip the per-word special-register "
                    "checks and bump their generation once per run (exact)");
REXCVAR_DEFINE_BOOL(masseffect_native_identity_memo, true, "Mass Effect",
                    "Shader identification memo by microcode content (type, XXH3, size): a program loaded again "
                    "after others skips the library matching; the draw-time identity guard is unchanged");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_exact_edges, true, "Mass Effect",
                    "EDRAM mode 4: a proven rectangle whose edges lie exactly on pixel boundaries (D3D pixel "
                    "centers, no window offset, w = 1) bounds the draw by itself, without the 1-pixel pad; the pad "
                    "made syncs transfer whole tile rows/columns the draw never touches");
REXCVAR_DEFINE_BOOL(masseffect_native_edram4_overwrite, true, "Mass Effect",
                    "EDRAM mode 4: skip the ownership transfer of tiles that a proven full-screen rectangle "
                    "overwrites completely (depth ALWAYS without stencil, or opaque full-mask color)");
// Ring-thread CPU switches: every one keeps the old path and checks itself against it for the first
// masseffect_native_verify_n uses (a DIFFERENCE line in the log and the old path for the rest of the session).
REXCVAR_DEFINE_BOOL(masseffect_native_flat_identity, false, "Mass Effect",
                    "Ring: shader identity checks (VS fetch-patch match, PS microcode match) memoized in small "
                    "direct-mapped tables keyed by (library entry, load generation) instead of an unordered_map and a "
                    "word-by-word compare per use; exact (pure function of the loaded microcode and the entry)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_fast_pair, false, "Mass Effect",
                    "Ring: draw/record pairing without the cold cache lines and locks: record start addresses in a "
                    "compact array (the best record of a packet is found in two 32-byte rows), object -> library entry "
                    "through a lock-free table, no draw-queue mutex per unpaired draw, prefetch while draining")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_raw_microcode, false, "Mass Effect",
                    "Ring: IM_LOAD of the same address and size as the last load of that stage compares the guest "
                    "words against a raw copy of the last load (one memcmp) instead of byte-swapping them first; "
                    "exact (the swap is a bijection)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_pm4_fast, false, "Mass Effect",
                    "Ring: runs of constant/fetch register writes (type 0, SET_CONSTANT2/SET_SHADER_CONSTANTS, "
                    "LOAD_ALU_CONSTANT) are byte-swapped, compared and stored four words at a time with NEON "
                    "straight from the guest words; same register contents and generation bumps")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_verify_n, 2048, "Mass Effect",
                     "Self-check of the switches above: the old and the new path are both evaluated for the first N "
                     "uses and compared; any difference logs DIFFERENCE and turns that switch off. 0 = no check")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(masseffect_native_vs_identity_tolerance, 0, "Mass Effect",
                     "Experimental: a vertex shader the ring loaded may differ from its library candidate (same size, "
                     "fetches still checked) in up to N ALU/CF words and still be drawn with that candidate. 0 = "
                     "exact identity (the draw is dropped when no candidate matches, as before). Try 4-16 when the "
                     "log shows 'unresolved draw shader pairs' (e.g. VS CD057930742AFE84, 120 words, one draw per "
                     "frame in the pause menu); see the '[native] VS word diff' lines")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::native {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;

// GPU MMIO window and register file.
constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegScratchUmsk = 0x01DC;
constexpr uint32_t kRegScratchAddr = 0x01DD;
constexpr uint32_t kRegScratch0 = 0x0578;
constexpr uint32_t kRegScratch7 = 0x057F;
constexpr uint32_t kRegCoherStatusHost = 0x0A31;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
// Display gamma ramp registers. Native presentation owns the Xenos output path, so it must mirror
// the table written by Direct3D just like the SDK command processor does.
constexpr uint32_t kRegGammaFirst = 0x1921;       // DC_LUT_RW_MODE
constexpr uint32_t kRegGammaIndex = 0x1922;       // DC_LUT_RW_INDEX
constexpr uint32_t kRegGammaSequential = 0x1923;  // DC_LUT_SEQ_COLOR
constexpr uint32_t kRegGamma30 = 0x1925;          // DC_LUT_30_COLOR
constexpr uint32_t kRegGammaMask = 0x1927;        // DC_LUT_WRITE_EN_MASK
constexpr uint32_t kRegGammaLast = 0x1927;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;
constexpr uint32_t kRegVgtEventInitiator = 0x21F9;
constexpr uint32_t kRegVgtDmaBase = 0x21FA;
constexpr uint32_t kRegVgtDmaSize = 0x21FB;
constexpr uint32_t kRegVgtDrawInitiator = 0x21FC;
constexpr uint32_t kRegRbModeControl = 0x2208;
constexpr uint32_t kRegRbSampleCountAddr = 0x2325;
constexpr int kMaxIndirectDepth = 4;
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint16_t kExtentMax = 2048 >> 3;
// Registers whose change invalidates viewport/raster state.
constexpr uint32_t kRegViewportFirst = 0x2080;
constexpr uint32_t kRegViewportLast = 0x2302;
constexpr uint32_t kRegFetchFirst = 0x4800;
constexpr uint32_t kRegFetchLast = 0x48BF;
inline bool IsViewportRegister(uint32_t i) {
  if (i - kRegViewportFirst > kRegViewportLast - kRegViewportFirst) return false;
  return i <= 0x2082 || (i >= 0x210F && i <= 0x2114) || (i >= 0x2204 && i <= 0x2206) || i == 0x2302;
}

// Big-endian words of a command buffer; the ring wraps (power-of-two size), an indirect buffer
// does not.
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t physical = 0;  // physical address of word 0 (to key packets by address)
  uint32_t mask = 0;
  uint32_t pos = 0;
  uint32_t end = 0;
  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t v = Peek();
    Advance(1);
    return v;
  }
  void Advance(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t value, uint32_t ref) {
  switch (info & 0x7) {
    case 0x0: return false;
    case 0x1: return value < ref;
    case 0x2: return value <= ref;
    case 0x3: return value == ref;
    case 0x4: return value != ref;
    case 0x5: return value >= ref;
    case 0x6: return value > ref;
    default: return true;
  }
}

// Physical microcode address -> library entry, filled on the game thread by NoteShaderObject and read
// by the ring thread on IM_LOAD.
std::mutex g_by_address_mutex;
std::unordered_map<uint32_t, const masseffect::native::ShaderEntry*> g_by_address;
std::unordered_set<uint32_t> g_objects_seen;
// Immediate loads (IM_LOAD_IMMEDIATE) carry no address: they are found by the XXH3 of the microcode
// (host order) of the objects the game binds, the VS one taken at draw time (already patched). Hashes
// that two different entries share are counted and dropped.
std::unordered_map<uint64_t, const masseffect::native::ShaderEntry*> g_by_code;
std::unordered_set<uint32_t> g_code_mapped;  // objects whose (current) microcode is in g_by_code
std::unordered_map<uint32_t, const masseffect::native::ShaderEntry*> g_entry_of_object;
uint64_t g_code_conflicts = 0;

// masseffect_native_fast_pair: g_entry_of_object (written by the game thread under g_by_address_mutex, never erased)
// mirrored in a lock-free table the ring reads without the mutex (me_object_table.h).
ObjectEntryTable<const masseffect::native::ShaderEntry*> g_objects;
inline void PublishObject(uint32_t object, const masseffect::native::ShaderEntry* entry) { g_objects.Publish(object, entry); }
inline bool LookupObject(uint32_t object, const masseffect::native::ShaderEntry*& entry) { return g_objects.Lookup(object, entry); }

// Draw records from the game thread (NoteDrawCall).
struct DrawRecord {
  uint32_t type, r5, r6, r7, vs, ps;
  uint32_t start = 0;  // physical address of Direct3D's command buffer write pointer before the call
};
// The ring runs several frames behind the game thread (~800 records measured), so records are never aged
// out; the window search drops the ones skipped when a later one matches.

// Index/vertex count the ring carries for `prims` primitives of a Xenos primitive type
// (sub_82228568 takes a primitive count and multiplies it, mullw at +175).
uint32_t CountForPrimitives(uint32_t type, uint32_t prims) {
  switch (type) {
    case 1: return prims;             // point list
    case 2: return prims * 2;         // line list
    case 3: return prims + 1;         // line strip
    case 4: return prims * 3;         // triangle list
    case 5: case 6: return prims + 2; // triangle fan, strip
    case 8: return prims * 3;         // rectangle list
    case 13: return prims * 4;        // quad list
    default: return prims;
  }
}
std::mutex g_draw_mutex;
std::deque<DrawRecord> g_draw_queue;
// The same records keyed by where Direct3D was about to write (device+48, the command buffer write
// pointer): the ring finds the record of a DRAW_INDX by its packet address, whatever the order.
std::map<uint32_t, DrawRecord> g_draw_by_address;
// The same index without allocations (masseffect_native_registers_table): 8 KB address buckets of 8 records;
// a lookup checks the packet's bucket and the previous one (records are at most 0x2000 below the packet).
// (me_record_table.h: the buckets, with a dense array of start addresses beside the slots for masseffect_native_fast_pair.)
DrawRecordTable<DrawRecord> g_records;
using DrawRecordSlot = DrawRecordTable<DrawRecord>::Slot;
bool RecordTable() {
  static const bool table = REXCVAR_GET(masseffect_native_registers_table);
  return table;
}
// Draw records from the game thread to the ring thread without the shared mutex (the record table is then
// owned by the ring thread alone): a bounded multi-producer queue (Vyukov) of (record, start). The ring
// drains it into g_record_table before every lookup. The ring runs a few frames behind (~800 records),
// so 16384 slots never fill in practice; a full queue makes the producer wait.
struct QueuedRecord {
  std::atomic<uint64_t> sequence{0};
  DrawRecord record;
  uint32_t start = 0;
};
constexpr uint64_t kRecordQueueSize = 16384;
std::vector<QueuedRecord> g_record_queue(kRecordQueueSize);
std::atomic<uint64_t> g_record_queue_tail{0};  // next slot to fill (producers)
uint64_t g_record_queue_head = 0;               // next slot to drain (ring thread only)
bool g_record_queue_ready = [] {
  for (uint64_t i = 0; i < kRecordQueueSize; ++i) g_record_queue[i].sequence.store(i, std::memory_order_relaxed);
  return true;
}();

bool g_record_fast = false;  // masseffect_native_fast_pair (set when the graphics system starts)

void InsertRecordInTable(DrawRecord&& record, uint32_t start) {
  g_records.Insert(std::move(record), start, g_record_fast);
}

void PushRecord(DrawRecord&& record, uint32_t start) {
  uint64_t pos = g_record_queue_tail.load(std::memory_order_relaxed);
  for (;;) {
    QueuedRecord& q = g_record_queue[pos & (kRecordQueueSize - 1)];
    const uint64_t seq = q.sequence.load(std::memory_order_acquire);
    if (seq == pos) {
      if (g_record_queue_tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
        q.record = std::move(record);
        q.start = start;
        q.sequence.store(pos + 1, std::memory_order_release);
        return;
      }
    } else if (seq < pos) {
      std::this_thread::yield();  // full: the ring thread has not drained yet
      pos = g_record_queue_tail.load(std::memory_order_relaxed);
    } else {
      pos = g_record_queue_tail.load(std::memory_order_relaxed);
    }
  }
}

// Ring thread: moves every queued record into the table, in order.
void DrainRecords() {
  for (;;) {
    QueuedRecord& q = g_record_queue[g_record_queue_head & (kRecordQueueSize - 1)];
    if (q.sequence.load(std::memory_order_acquire) != g_record_queue_head + 1) return;
    if (g_record_fast) {  // the next records were written by the game thread's core: start pulling their lines
      __builtin_prefetch(&g_record_queue[(g_record_queue_head + 1) & (kRecordQueueSize - 1)]);
      __builtin_prefetch(&g_record_queue[(g_record_queue_head + 2) & (kRecordQueueSize - 1)]);
    }
    InsertRecordInTable(std::move(q.record), q.start);
    q.sequence.store(g_record_queue_head + kRecordQueueSize, std::memory_order_release);
    ++g_record_queue_head;
  }
}
std::atomic<bool> g_native_active{false};
std::atomic<uint64_t> g_draw_records{0};

uint64_t CodeHash(const uint8_t* big_endian, uint32_t words) {
  std::vector<uint32_t> host(words);
  for (uint32_t i = 0; i < words; ++i) host[i] = rex::memory::load_and_swap<uint32_t>(big_endian + i * 4);
  return XXH3_64bits(host.data(), host.size() * sizeof(uint32_t));
}

// Whole-library index by unmasked microcode (host order), unique hashes only: for immediate loads of
// shaders the game never binds through the device (Direct3D's own clear/copy shaders).
std::unordered_map<uint64_t, const masseffect::native::ShaderEntry*> g_library_by_code;

bool OriginalMicrocode(const masseffect::native::ShaderEntry& entry, uint32_t& virtual_size,
                       uint32_t& offset, uint32_t& bytes, std::vector<uint32_t>* words = nullptr) {
  if (!entry.shader || entry.shader->original.size() < 28) return false;
  const auto& o = entry.shader->original;
  virtual_size = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
  const uint32_t shader_header = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
  if (virtual_size > o.size() || shader_header + 8 > virtual_size) return false;
  offset = rex::memory::load_and_swap<uint32_t>(o.data() + shader_header);
  bytes = rex::memory::load_and_swap<uint32_t>(o.data() + shader_header + 4);
  if (!bytes || (bytes & 3) || size_t(virtual_size) + offset + bytes > o.size()) return false;
  if (words) {
    words->resize(bytes / 4);
    for (size_t i = 0; i < words->size(); ++i) {
      (*words)[i] = rex::memory::load_and_swap<uint32_t>(o.data() + virtual_size + offset + i * 4);
    }
  }
  return true;
}

// Direct3D's clear/copy/video shaders are loaded with IM_LOAD_IMMEDIATE after link-time rewriting.
// Their original containers are in the library, but the exact ring microcode is not. Materialize a
// stable container from the closest register-linked template so the normal offline translator can add
// the variant on the next run. Package shaders (with a constant table) are deliberately not candidates.
void DumpMissingImmediate(const masseffect::native::ShadersNative& library, bool vertex,
                          const std::vector<uint32_t>& microcode) {
  const char* folder = std::getenv("MASSEFFECT_SHADER_MISSING");
  if (!folder || !*folder || microcode.empty()) return;
  const uint64_t code_hash = XXH3_64bits(microcode.data(), microcode.size() * sizeof(uint32_t)) ^
                             (vertex ? 1ull : 0ull);
  static std::mutex mutex;
  static std::unordered_set<uint64_t> dumped;
  std::lock_guard<std::mutex> lock(mutex);
  if (!dumped.insert(code_hash).second) return;

  const masseffect::native::ShaderEntry* best = nullptr;
  uint64_t best_score = UINT64_MAX;
  uint32_t best_vsize = 0, best_offset = 0, best_bytes = 0, best_different = 0;
  for (uint32_t n = 0;; ++n) {
    const auto* candidate = library.PerNumber(n);
    if (!candidate) break;
    if (candidate->vertices != vertex || !candidate->binding_per_register) continue;
    uint32_t vsize = 0, offset = 0, bytes = 0;
    std::vector<uint32_t> original;
    if (!OriginalMicrocode(*candidate, vsize, offset, bytes, &original) ||
        original.size() != microcode.size()) {
      continue;
    }
    uint32_t different = 0;
    uint64_t bits = 0;
    for (size_t i = 0; i < original.size(); ++i) {
      const uint32_t delta = original[i] ^ microcode[i];
      different += delta != 0;
      bits += std::popcount(delta);
    }
    const uint64_t score = uint64_t(different) * 1000 + bits;
    if (score < best_score) {
      best = candidate;
      best_score = score;
      best_vsize = vsize;
      best_offset = offset;
      best_bytes = bytes;
      best_different = different;
    }
  }
  if (!best || !best->shader) {
    REXLOG_WARN("[native] no internal {} template for an immediate shader of {} words",
                vertex ? "VS" : "PS", microcode.size());
    return;
  }
  std::vector<uint8_t> container = best->shader->original;
  for (size_t i = 0; i < microcode.size(); ++i) {
    rex::memory::store_and_swap<uint32_t>(container.data() + best_vsize + best_offset + i * 4,
                                          microcode[i]);
  }
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const uint64_t h = XXH3_64bits(container.data(), container.size());
  const auto path = std::filesystem::path(folder) /
                    fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", h);
  if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
    std::fwrite(container.data(), 1, container.size(), f);
    std::fclose(f);
    REXLOG_INFO("[native] immediate {} variant kept: {} (template n{}, {} of {} words changed)",
                vertex ? "VS" : "PS", path.string(), best->number, best_different,
                best_bytes / 4);
  }
}

// Lossless discovery artifact for every shader which the current library cannot identify. Unlike
// DumpMissingImmediate this deliberately does not borrow a container or guess an identity: the file
// is the exact big-endian microcode from the ring plus a small human-readable sidecar. It is input
// for instruction-aware classification of Direct3D's internal shaders.
void DumpRawMicrocode(bool vertex, uint32_t address, const std::vector<uint32_t>& microcode) {
  const char* folder = std::getenv("MASSEFFECT_SHADER_DISCOVERY");
  if (!folder || !*folder || microcode.empty()) return;
  const uint64_t hash = XXH3_64bits(microcode.data(), microcode.size() * sizeof(uint32_t));
  const uint64_t key = hash ^ (vertex ? 1ull : 0ull);
  static std::mutex mutex;
  static std::unordered_set<uint64_t> dumped;
  std::lock_guard<std::mutex> lock(mutex);
  if (!dumped.insert(key).second) return;

  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const std::string stem = fmt::format("{}_{:016x}", vertex ? "vs" : "ps", hash);
  const auto raw_path = std::filesystem::path(folder) / (stem + ".ucode");
  if (std::FILE* f = std::fopen(raw_path.string().c_str(), "wb")) {
    for (uint32_t word : microcode) {
      uint8_t bytes[4] = {uint8_t(word >> 24), uint8_t(word >> 16), uint8_t(word >> 8),
                          uint8_t(word)};
      std::fwrite(bytes, 1, sizeof(bytes), f);
    }
    std::fclose(f);
  }
  const auto text_path = std::filesystem::path(folder) / (stem + ".txt");
  if (std::FILE* f = std::fopen(text_path.string().c_str(), "w")) {
    std::fprintf(f, "stage=%s\naddress=%08X\nwords=%zu\nhash=%016llX\n", vertex ? "vs" : "ps",
                 address, microcode.size(), static_cast<unsigned long long>(hash));
    for (size_t i = 0; i < microcode.size(); ++i) std::fprintf(f, "%04zu %08X\n", i, microcode[i]);
    std::fclose(f);
  }
  REXLOG_INFO("[native] raw unidentified {} kept: {} ({} words)", vertex ? "VS" : "PS",
              raw_path.string(), microcode.size());
}

void BuildLibraryCodeIndex(const masseffect::native::ShadersNative& library) {
  std::unordered_set<uint64_t> shared;
  auto register_link_score = [](const masseffect::native::ShaderEntry* e) {
    if (!e || !e->binding_per_register || !e->shader) return uint32_t(0);
    // Current raw-PS wrappers carry all 16 register-linked interpolator mappings after the
    // 32-byte shader header. Older discovery wrappers stopped at the header, so XenosRecomp
    // initialized the missing inputs to zero (in particular UI alpha). Prefer the complete
    // wrapper when several containers contain byte-for-byte identical microcode.
    if (!e->vertices && e->shader->original.size() >= 28) {
      const auto& o = e->shader->original;
      const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
      const uint32_t sh = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
      if (sh <= vsize && vsize - sh >= 32 + 16 * 4) return uint32_t(3);
    }
    return uint32_t(2);
  };
  for (uint32_t n = 0;; ++n) {
    const masseffect::native::ShaderEntry* e = library.PerNumber(n);
    if (!e) break;
    if (!e->shader) continue;
    const auto& o = e->shader->original;
    if (o.size() < 28) continue;
    const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
    const uint32_t sh = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
    if (sh + 8 > vsize || vsize > o.size()) continue;
    const uint32_t off = rex::memory::load_and_swap<uint32_t>(o.data() + sh);
    const uint32_t bytes = rex::memory::load_and_swap<uint32_t>(o.data() + sh + 4);
    if (size_t(vsize) + off + bytes > o.size() || !bytes) continue;
    const uint64_t h = CodeHash(o.data() + vsize + off, bytes / 4) ^ (e->vertices ? 1 : 0);
    auto [slot, inserted] = g_library_by_code.emplace(h, e);
    if (!inserted) {
      shared.insert(h);
      if (register_link_score(e) > register_link_score(slot->second)) slot->second = e;
    }
  }
  // Hashes several containers share are kept (first entry): identical microcode reads the same
  // constant and fetch registers, only the constant table names differ.
  REXLOG_INFO("[native] library microcode index: {} entries, {} shared by more than one container",
              g_library_by_code.size(), shared.size());
}

const masseffect::native::ShaderEntry* ByCode(const std::vector<uint32_t>& microcode, bool vertex,
    const masseffect::native::ShaderEntry** observed = nullptr) {
  const uint64_t h = XXH3_64bits(microcode.data(), microcode.size() * sizeof(uint32_t));
  const masseffect::native::ShaderEntry* entry = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    auto it = g_by_code.find(h);
    if (it != g_by_code.end()) entry = it->second;
  }
  if (!entry) {
    auto it = g_library_by_code.find(h ^ (vertex ? 1 : 0));
    if (it != g_library_by_code.end()) entry = it->second;
  }
  if (observed) *observed = entry;  // Preserve rejected candidates for bounded diagnostics.
  if (!entry) return nullptr;
  // VS fetch-patch compatibility remains checked by IdentifyShader, unchanged.
  if (vertex) return entry->vertices ? entry : nullptr;
  if (!PixelShaderIdentityMatches(
      {ShaderIdentityStage::Pixel, microcode},
      {entry->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel,
       entry->microcode})) return nullptr;
  return entry;
}
std::atomic<const masseffect::native::ShadersNative*> g_library{nullptr};
// Vertex shaders seen only after Direct3D patched their microcode (sub_8222F1C8) are found by the
// virtual part of their container (header, constant table, vertex elements); hashes shared by more
// than one library entry are left out.
std::unordered_map<uint64_t, const masseffect::native::ShaderEntry*> g_vs_by_virtual;
bool g_vs_by_virtual_built = false;

void BuildVirtualIndex(const masseffect::native::ShadersNative* library) {
  std::unordered_set<uint64_t> shared;
  for (uint32_t n = 0;; ++n) {
    const masseffect::native::ShaderEntry* e = library->PerNumber(n);
    if (!e) break;
    if (!e->vertices || !e->shader || e->shader->original.size() < 8) continue;
    const auto& o = e->shader->original;
    const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
    if (vsize > o.size()) continue;
    const uint64_t h = XXH3_64bits(o.data(), vsize);
    if (!g_vs_by_virtual.emplace(h, e).second) shared.insert(h);
  }
  for (uint64_t h : shared) g_vs_by_virtual.erase(h);
  g_vs_by_virtual_built = true;
}

const masseffect::native::ShaderEntry* ByAddress(uint32_t physical) {
  std::lock_guard<std::mutex> lock(g_by_address_mutex);
  auto it = g_by_address.find(physical);
  return it == g_by_address.end() ? nullptr : it->second;
}

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem() : registers_(kRegisterCount, 0) {}
  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) return X_STATUS_SUCCESS;
    app_context_ = app_context;
    if (!provider_) {
      // The XenosRecomp SPIR-V needs device features that must be requested at creation.
      rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (!provider_) {
        REXLOG_ERROR("[native] could not create the Vulkan device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[native] native graphics system: no Xenos emulation");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ = memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this,
                                                        &MmioRead, &MmioWrite);
      if (!mmio_registered_) {
        REXLOG_ERROR("[native] could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) return X_STATUS_SUCCESS;
    g_vs_identity_alu_tolerance.store(uint32_t(REXCVAR_GET(masseffect_native_vs_identity_tolerance)),
                                      std::memory_order_relaxed);
    identity_flat_ = REXCVAR_GET(masseffect_native_flat_identity);
    g_record_fast = pairing_fast_ = REXCVAR_GET(masseffect_native_fast_pair);
    raw_microcode_ = REXCVAR_GET(masseffect_native_raw_microcode);
    pm4_fast_ = REXCVAR_GET(masseffect_native_pm4_fast);
    check_identity_ = check_pairing_ = check_raw_ = check_pm4_ = check_objects_ =
        uint32_t(std::max<int32_t>(0, REXCVAR_GET(masseffect_native_verify_n)));
    if (identity_flat_ || pairing_fast_ || raw_microcode_ || pm4_fast_)
      REXLOG_INFO("[native] ring CPU switches: flat identity {}, fast pairing {}, raw microcode compare {}, fast PM4 "
                  "runs {}; self-check of the first {} uses of each", identity_flat_, pairing_fast_, raw_microcode_,
                  pm4_fast_, check_identity_);
    g_native_active.store(true, std::memory_order_release);
    const char* lib = std::getenv("MASSEFFECT_SHADER_LIBRARY");
    const auto lib_path = lib && *lib ? std::filesystem::path(lib)
                                      : rex::filesystem::GetExecutableFolder() / "masseffect_shaders.mesp";
    if (shaders_.Load(lib_path, REXCVAR_GET(masseffect_shaders_index))) {
      masseffect::native::g_active_library = &shaders_;
      BuildLibraryCodeIndex(shaders_);
      g_library.store(&shaders_, std::memory_order_release);
      REXLOG_INFO("[native] shader library {}", lib_path.string());
    } else {
      REXLOG_WARN("[native] no shader library at {}: nothing will be drawn", lib_path.string());
    }
    start_ = last_report_ = Clock::now();
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU VSync native");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring native");
    ring_thread_->Create();
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[native] ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_writeback_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    g_native_active.store(false, std::memory_order_release);
    if (active_.exchange(false)) {
      { std::lock_guard<std::mutex> lock(ring_mutex_); }
      ring_cv_.notify_all();
      if (ring_thread_) {
        ring_thread_->Wait(0, 0, 0, nullptr);
        ring_thread_.reset();
      }
      if (vblank_thread_) {
        vblank_thread_->Wait(0, 0, 0, nullptr);
        vblank_thread_.reset();
      }
      Report(true);
    }
    draw_extent_estimator_.reset();
    extent_diagnostics_.clear();
    targets_.reset();
    if (presenter_) {
      if (app_context_) app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- MMIO ---
  static uint32_t MmioRead(void*, void* ctx, uint32_t address) {
    return static_cast<NativeGraphicsSystem*>(ctx)->ReadMmio(address);
  }
  static void MmioWrite(void*, void* ctx, uint32_t address, uint32_t value) {
    static_cast<NativeGraphicsSystem*>(ctx)->WriteMmio(address, value);
  }

  uint32_t ReadMmio(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming: return 0x08100748;
      case kRegRbBcControl: return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus: return 1;
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return (std::min(uint32_t(mode.display_width), uint32_t(0x0FFF)) << 16) |
               std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      default: break;
    }
    return r < kRegisterCount ? registers_[r] : 0;
  }

  void WriteMmio(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      // Wake the ring thread only when it waits (an empty lock + notify on every kick cost the render
      // thread ~2 %). seq_cst on both sides (here and ring_waiting_ in RingLoop): either the ring thread's
      // predicate sees the new pointer or this load sees it waiting; its 5 ms timeout bounds any miss.
      write_pointer_.store(value, std::memory_order_seq_cst);
      if (ring_waiting_.load(std::memory_order_seq_cst)) {
        { std::lock_guard<std::mutex> lock(ring_mutex_); }
        ring_cv_.notify_one();
      }
    }
    if (r < kRegisterCount) registers_[r] = value;  // no side effects through MMIO
  }

  void NoteGammaRampWrite(uint32_t index, uint32_t value) {
    ++gamma_writes_;
    if (index == kRegGammaIndex) {
      gamma_component_ = 0;
    } else if (index == kRegGammaMask) {
      gamma_mask_ = value & 0b111;
    } else if (index == kRegGammaSequential) {
      const uint32_t i = registers_[kRegGammaIndex] & 0xFF;
      if (gamma_mask_ & (UINT32_C(1) << (2 - gamma_component_))) {
        gamma_ramp_[i][gamma_component_] = uint16_t((value >> 6) & 0x3FF);
        ++gamma_version_;
      }
      if (++gamma_component_ >= 3) {
        gamma_component_ = 0;
        registers_[kRegGammaIndex] =
            (registers_[kRegGammaIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
      }
    } else if (index == kRegGamma30) {
      const uint32_t i = registers_[kRegGammaIndex] & 0xFF;
      if (gamma_mask_ & 0b001) gamma_ramp_[i][2] = uint16_t(value & 0x3FF);
      if (gamma_mask_ & 0b010) gamma_ramp_[i][1] = uint16_t((value >> 10) & 0x3FF);
      if (gamma_mask_ & 0b100) gamma_ramp_[i][0] = uint16_t((value >> 20) & 0x3FF);
      if (gamma_mask_) ++gamma_version_;
      gamma_component_ = 0;
      registers_[kRegGammaIndex] =
          (registers_[kRegGammaIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
    }
  }

  // --- Registers written by the ring ---
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) return;
    if (index == kRegCoherStatusHost) value |= 0x80000000u;
    if (registers_[index] != value) {  // generations: what a draw must re-upload
      if (index >= 0x4000 && index < 0x4800) {
        ++(index < 0x4400 ? gen_constants_vs_ : gen_constants_ps_);
      } else if (index >= kRegFetchFirst && index <= kRegFetchLast) {
        ++gen_fetch_;
      } else if (IsViewportRegister(index)) {
        ++gen_viewport_;
      }
    }
    if (index >= kRegGammaFirst && index <= kRegGammaLast) {
      NoteGammaRampWrite(index, value);
    }
    registers_[index] = value;
    if (index >= kRegScratch0 && index <= kRegScratch7) {
      const uint32_t n = index - kRegScratch0;
      if ((1u << n) & registers_[kRegScratchUmsk]) {
        rex::memory::store_and_swap<uint32_t>(
            memory_->TranslatePhysical(registers_[kRegScratchAddr] + n * 4), value);
      }
    }
  }

  // A run of register words into the constant/fetch range (0x4000-0x48BF), the bulk of every draw's writes:
  // no per-word special-register checks, and each generation bumped once if any word of its part changed.
  template <typename Next>
  void WriteConstantRun(uint32_t index, uint32_t count, Next next) {
    if (!REXCVAR_GET(masseffect_native_fast_registers) || index < 0x4000 ||
        uint64_t(index) + count > uint64_t(kRegFetchLast) + 1) {
      for (uint32_t i = 0; i < count; ++i) WriteRegister(index + i, next());
      return;
    }
    bool vs = false, ps = false, fetch = false;
    uint32_t* regs = registers_.data();
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t r = index + i, v = next();
      if (regs[r] != v) {
        regs[r] = v;
        if (r < 0x4400) vs = true;
        else if (r < 0x4800) ps = true;
        else fetch = true;
      }
    }
    if (vs) ++gen_constants_vs_;
    if (ps) ++gen_constants_ps_;
    if (fetch) ++gen_fetch_;
  }

  // The count guest words (big-endian, as the Reader sees them) at the reader position, if they are contiguous in
  // memory (indirect buffers never wrap; the ring only at its end).
  static const uint32_t* ContiguousWords(const Reader& data, uint32_t count) {
    if (data.mask && uint64_t(data.pos) + count > uint64_t(data.mask) + 1) return nullptr;
    return reinterpret_cast<const uint32_t*>(data.base + size_t(data.pos) * 4);
  }

  // WriteConstantRun over guest words (big-endian) in memory: the constant/fetch range (0x4000-0x48BF) is compared and
  // stored four words at a time; each generation is bumped once if any word of its part changed. Outside that range
  // (or with the fast path off) every word goes through WriteRegister as before.
  void WriteConstantRunRaw(uint32_t index, uint32_t count, const uint32_t* guest) {
    if (!REXCVAR_GET(masseffect_native_fast_registers) || index < 0x4000 ||
        uint64_t(index) + count > uint64_t(kRegFetchLast) + 1) {
      for (uint32_t i = 0; i < count; ++i) WriteRegister(index + i, __builtin_bswap32(guest[i]));
      return;
    }
    uint32_t* regs = registers_.data();
    std::vector<uint32_t> before;
    const bool check = check_pm4_ > 0;
    if (check) {
      --check_pm4_;
      before.assign(regs + index, regs + index + count);
    }
    __builtin_prefetch(guest + count);
    __builtin_prefetch(guest + count + 16);
    const RunChange change = ApplyRunRaw(regs, index, count, guest);
    if (change.vs) ++gen_constants_vs_;
    if (change.ps) ++gen_constants_ps_;
    if (change.fetch) ++gen_fetch_;
    if (check) {  // the original loop on the saved words
      std::vector<uint32_t> expected(before);
      uint32_t i = 0;
      const RunChange want = ApplyRunReference(expected.data(), index, count,
                                               [&] { return __builtin_bswap32(guest[i++]); });
      if (!(want == change) || !std::equal(expected.begin(), expected.end(), regs + index)) {
        REXLOG_ERROR("[native] DIFFERENCE: fast PM4 register run (index {:04X} count {}): generations vs {}/{} ps {}/{} "
                     "fetch {}/{} or values differ; the old path from now on", index, count, change.vs, want.vs, change.ps,
                     want.ps, change.fetch, want.fetch);
        pm4_fast_ = false;
        for (uint32_t k = 0; k < count; ++k) regs[index + k] = __builtin_bswap32(guest[k]);
      }
    }
  }

  uint32_t ReadMemory(uint32_t address) const {
    uint32_t v;
    std::memcpy(&v, memory_->TranslatePhysical(address & ~3u), 4);
    return xenos::GpuSwap(v, static_cast<xenos::Endian>(address & 0x3));
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    const uint32_t v = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~3u), &v, 4);
  }

  // --- Threads ---
  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
    interrupts_.fetch_add(1, std::memory_order_relaxed);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      if (now - next > std::chrono::milliseconds(250)) next = now;
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      if (REXCVAR_GET(masseffect_vblank_sleep_exact)) {
        // Sleep to the next vblank instead of waking up 1000 times a second (60 of them useful).
        const auto wait = std::chrono::duration_cast<std::chrono::microseconds>(next - Clock::now());
        rex::thread::Sleep(std::clamp(wait, std::chrono::microseconds(200), std::chrono::microseconds(50000)));
      } else {
        rex::thread::Sleep(std::chrono::milliseconds(1));
      }
      REX_SYS_COUNT(rex::syscount::kVblankTick, 1);
    }
    return 0;
  }

  int RingLoop() {
    uint32_t read = 0;
    uint32_t generation = ring_generation_.load(std::memory_order_acquire);
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_waiting_.store(true, std::memory_order_seq_cst);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(5), [&] {
          const uint32_t words = ring_words_.load(std::memory_order_acquire);
          return !active_.load(std::memory_order_acquire) ||
                 (words && (write_pointer_.load(std::memory_order_seq_cst) & (words - 1)) != read);
        });
        ring_waiting_.store(false, std::memory_order_relaxed);
      }
      if (!active_.load(std::memory_order_acquire)) break;
      Report(false);
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) continue;
      const uint32_t written = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) continue;
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.physical = base;
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      while (reader.Pending()) {
        if (!Packet(reader, 0)) break;  // partial packet: the rest comes with the next WPTR
      }
      read = reader.pos;
      const uint32_t writeback = read_writeback_.load(std::memory_order_acquire);
      if (writeback) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
      }
      NotifyRingProgress();
      masseffect::native::deferred::Flush();  // Batched mode: the worker must not wait for the next WPTR
    }
    return 0;
  }

  // --- PM4 ---
  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Peek();
    uint32_t count = 0;
    switch (packet >> 30) {
      case 0: count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0; break;
      case 1: count = 2; break;
      case 2: count = 0; break;
      default: count = ((packet >> 16) & 0x3FFF) + 1; break;
    }
    if (reader.Pending() < count + 1) return false;
    if (pm4_fast_) {  // the stream was written by the game thread's core: pull the lines ahead of the parse
      const uint32_t ahead = reader.pos + count + 1 + 48;
      __builtin_prefetch(reader.base + size_t(reader.mask ? (ahead & reader.mask) : ahead) * 4);
    }
    packet_address_ = reader.physical + reader.pos * 4;
    reader.Advance(1);
    Reader data = reader;
    reader.Advance(count);
    ++packets_;
    if (!packet) return true;
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool single = (packet >> 15) & 0x1;
        ++type0_packets_;
        type0_words_ += count;
        type0_single_ += single;
        if (single) {
          for (uint32_t i = 0; i < count; ++i) WriteRegister(index, data.Read());
        } else if (const uint32_t* words = pm4_fast_ ? ContiguousWords(data, count) : nullptr) {
          WriteConstantRunRaw(index, count, words);
        } else {
          WriteConstantRun(index, count, [&] { return data.Read(); });
        }
        break;
      }
      case 1: {
        const uint32_t v1 = data.Read(), v2 = data.Read();
        WriteRegister(packet & 0x7FF, v1);
        WriteRegister((packet >> 11) & 0x7FF, v2);
        break;
      }
      case 2: break;
      default: Type3(data, packet, count, depth); break;
    }
    return true;
  }

  static uint32_t ConstantBase(uint32_t type) {
    switch (type) {
      case 0: return 0x4000;  // ALU
      case 1: return 0x4800;  // fetch
      case 2: return 0x4900;  // bool
      case 3: return 0x4908;  // loop
      case 4: return 0x2000;  // registers
      default: return UINT32_MAX;
    }
  }

  void Type3(Reader& data, uint32_t packet, uint32_t words, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    ++opcodes_[opcode];
    opcode_words_[opcode] += words;
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      // A draw the predicate drops still had its Draw* record: consume it so the pairing stays in step.
      if ((opcode == xenos::PM4_DRAW_INDX || opcode == xenos::PM4_DRAW_INDX_2) &&
          words >= (opcode == xenos::PM4_DRAW_INDX ? 2u : 1u)) {
        if (opcode == xenos::PM4_DRAW_INDX) data.Read();
        const uint32_t initiator = data.Read();
        const uint32_t saved = registers_[kRegVgtDrawInitiator];
        registers_[kRegVgtDrawInitiator] = initiator;
        PairDraw();
        registers_[kRegVgtDrawInitiator] = saved;
        ++draws_predicated_;
      }
      return;
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (words < 1) break;
        const uint32_t cpus = data.Read();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) Interrupt(1, cpu);
        }
        break;
      }
      case xenos::PM4_XE_SWAP:
        if (words >= 4) {
          data.Read();
          data.Read();  // frontbuffer
          swap_width_ = data.Read();
          swap_height_ = data.Read();
        }
        ++swaps_;
        counter_.fetch_add(1, std::memory_order_relaxed);
        Present();
        break;
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (words < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxIndirectDepth) {
          Reader r;
          r.base = memory_->TranslatePhysical(address);
          r.physical = address;
          r.end = length;
          while (r.Pending()) {
            if (!Packet(r, depth + 1)) break;
          }
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (words < 5) break;
        const uint32_t info = data.Read(), poll = data.Read(), ref = data.Read(), mask = data.Read();
        const auto limit = Clock::now() + kWaitRegMemMax;
        for (;;) {
          if (!(info & 0x10) && poll == kRegCoherStatusHost && (registers_[poll] & 0x80000000u)) {
            registers_[poll] = 0;  // MakeCoherent: no shared memory to synchronize
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, ref)) break;
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (waits_timed_out_++ == 0) {
              REXLOG_WARN("[native] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, ref, mask);
            }
            break;
          }
          rex::thread::Sleep(std::chrono::microseconds(50));
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (words < 3) break;
        const uint32_t info = data.Read(), and_v = data.Read(), or_v = data.Read();
        uint32_t v = Register(info & 0x1FFF);
        v &= ((info >> 31) & 0x1) ? Register(and_v & 0x1FFF) : and_v;
        v |= ((info >> 30) & 0x1) ? Register(or_v & 0x1FFF) : or_v;
        WriteRegister(info & 0x1FFF, v);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (words < 2) break;
        const uint32_t reg = data.Read(), address = data.Read();
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (words < 1) break;
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < words; ++i, address += 4) WriteMemory(address, data.Read());
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (words < 6) break;
        const uint32_t info = data.Read(), poll = data.Read(), ref = data.Read(), mask = data.Read();
        const uint32_t dest = data.Read(), value = data.Read();
        const uint32_t v = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, v & mask, ref)) {
          if (info & 0x100) {
            WriteMemory(dest, value);
          } else {
            WriteRegister(dest, value);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (words < 1) break;
        WriteRegister(kRegVgtEventInitiator, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (words < 3) break;
        const uint32_t initiator = data.Read(), address = data.Read(), value = data.Read();
        WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1) ? counter_.load(std::memory_order_relaxed)
                                                       : value);
        // The D3D fence word the game's wait loop compares (sub_8222C768 / sub_8222FA98): wake it now, not
        // at the end of the ring segment.
        NotifyRingProgress();
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (words < 2) break;
        const uint32_t initiator = data.Read(), address = data.Read();
        WriteRegister(kRegVgtEventInitiator, initiator & 0x3F);
        const uint16_t extent[] = {0, kExtentMax, 0, kExtentMax, 0, 1};
        uint8_t* dest = memory_->TranslatePhysical(address & ~3u);
        for (size_t i = 0; i < std::size(extent); ++i) {
          rex::memory::store_and_swap<uint16_t>(dest + i * 2, extent[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (words < 1) break;
        WriteRegister(kRegVgtEventInitiator, data.Read() & 0x3F);
        // D3D9 occlusion queries on Xbox 360:
        // Issue(D3DISSUE_BEGIN): RB_SAMPLE_COUNT_ADDR points to query->begin (+0x20).
        // Issue(D3DISSUE_END):   RB_SAMPLE_COUNT_ADDR points to query->end   (+0x00).
        // D3D places the byte-swapped sentinel 0xFFFFFEED in the end structure before EVENT_WRITE_ZPD.
        const uint32_t address = Register(kRegRbSampleCountAddr);
        if (address) {
          auto* counts =
              memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
          const uint32_t finished = rex::byte_swap(uint32_t(0xFFFFFEED));
          const bool end = counts->ZPass_A == finished || counts->ZPass_B == finished ||
                           counts->ZFail_A == finished || counts->ZFail_B == finished;
          std::memset(counts, 0, sizeof(*counts));
          // SDK fallback: begin = 0, end = 1000 samples.
          if (end) counts->Total_A = counts->ZPass_A = 1000;
          static uint32_t query_reports = 0;
          if (query_reports++ < 30) {
            REXLOG_INFO("[native] ZPD {} at {:08X}: {} samples", end ? "end" : "begin", address,
                        uint32_t(counts->ZPass_A));
          }
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (words < 1) break;
        const uint32_t type_index = data.Read();
        const uint32_t base = ConstantBase((type_index >> 16) & 0xFF);
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 1; i < words; ++i) WriteRegister(index + i - 1, data.Read());
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        if (words < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        if (const uint32_t* raw = pm4_fast_ ? ContiguousWords(data, words - 1) : nullptr) {
          WriteConstantRunRaw(index, words - 1, raw);
        } else {
          WriteConstantRun(index, words - 1, [&] { return data.Read(); });
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        if (words < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t n = data.Read() & 0xFFF;
        const uint32_t base = ConstantBase((type_index >> 16) & 0xFF);
        if (base != UINT32_MAX) {
          const uint8_t* src = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          uint32_t i = 0;
          if (pm4_fast_) {
            WriteConstantRunRaw(index, n, reinterpret_cast<const uint32_t*>(src));
          } else {
            WriteConstantRun(index, n, [&] { return rex::memory::load_and_swap<uint32_t>(src + size_t(i++) * 4); });
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (words >= 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (words >= 1) bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (words >= 1) bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (words >= 1) bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK:
        if (words >= 2) {
          const uint64_t hi = data.Read(), lo = data.Read();
          bin_mask_ = (hi << 32) | lo;
        }
        break;
      case xenos::PM4_SET_BIN_SELECT:
        if (words >= 2) {
          const uint64_t hi = data.Read(), lo = data.Read();
          bin_select_ = (hi << 32) | lo;
        }
        break;
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (words < skip + 1) break;
        if (skip) data.Read();
        const uint32_t initiator = data.Read();
        WriteRegister(kRegVgtDrawInitiator, initiator);
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA) && words >= skip + 3) {
          WriteRegister(kRegVgtDmaBase, data.Read());
          WriteRegister(kRegVgtDmaSize, data.Read());
        }
        if ((Register(kRegRbModeControl) & 0x7) == uint32_t(xenos::EdramMode::kCopy)) {
          ++copies_;
          Copy();
        } else {
          ++draws_;
          if (depth > 0) ++draws_indirect_;
          PairDraw();
          if (draw_vs_ && (draw_ps_ || (Register(kRegRbModeControl) & 0x7) == 5)) {
            Draw();
          } else {
            ++draws_unidentified_;
            ++missing_draw_pairs_[{draw_vs_ ? 0 : current_vs_hash_, draw_ps_ ? 0 : current_ps_hash_}];
          }
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {
        if (words < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        im_load_address_ = address_type & 0x1FFFFFFCu;
        const uint8_t* src = memory_->TranslatePhysical(address_type & ~3u);
        const uint32_t load_type = address_type & 0x3;
        if (raw_microcode_ && load_type <= 1 && ReloadsSameMicrocode(load_type, src, size)) break;
        microcode_.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
        }
        IdentifyShader(load_type);
        if (raw_microcode_ && load_type <= 1) {
          // Every path of IdentifyShader leaves the stage's microcode equal to this load: keep its raw words.
          auto& raw = load_type == 1 ? raw_ps_ : raw_vs_;
          raw.assign(reinterpret_cast<const uint32_t*>(src), reinterpret_cast<const uint32_t*>(src) + size);
          (load_type == 1 ? raw_ps_valid_ : raw_vs_valid_) = true;
        }
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        if (words < 2) break;
        const uint32_t type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        if (size > words - 2) break;
        im_load_address_ = 0;  // immediate: the microcode is in the ring
        microcode_.resize(size);
        for (uint32_t i = 0; i < size; ++i) microcode_[i] = data.Read();
        IdentifyShader(type & 0x3);
        if ((type & 0x3) == 0) raw_vs_valid_ = false;  // the raw copy no longer matches the stage's microcode
        else if ((type & 0x3) == 1) raw_ps_valid_ = false;
        break;
      }
      default: break;  // IM_LOAD, IM_LOAD_IMMEDIATE, NOP, ...: nothing to do without drawing
    }
  }

  uint32_t Register(uint32_t index) const { return index < kRegisterCount ? registers_[index] : 0; }

  // --- Native drawing ---
  bool EnsureTargets() {
    if (!targets_ && !targets_failed_) {
      targets_ = masseffect::native::TargetsNative::Create(provider_ ? provider_->vulkan_device() : nullptr,
                                                        memory_);
      if (!targets_) {
        targets_failed_ = true;
        REXLOG_ERROR("[native] could not create the native render targets");
      }
    }
    return targets_ != nullptr;
  }

  bool ComputeVertexIdentity(const masseffect::native::ShaderEntry* candidate) const {
    return VertexShaderIdentityMatches(
        {ShaderIdentityStage::Vertex, vs_microcode_},
        {candidate->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel, candidate->microcode},
        candidate->elements, [](const auto& element) { return element.instruction; });
  }
  bool ComputePixelIdentity(const masseffect::native::ShaderEntry* candidate) const {
    return PixelShaderIdentityMatches(
        {ShaderIdentityStage::Pixel, ps_microcode_},
        {candidate->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel, candidate->microcode});
  }
  // Direct-mapped memo cell of a library entry for one stage's current load generation.
  struct IdentityCell {
    const masseffect::native::ShaderEntry* candidate = nullptr;
    uint64_t generation = 0;
    bool result = false;
  };
  static constexpr size_t kIdentityCells = 1024;
  static size_t IdentityCellOf(const void* entry) {
    uintptr_t x = uintptr_t(entry) >> 4;
    x ^= x >> 10;
    return size_t(x) & (kIdentityCells - 1);
  }
  // masseffect::native::FetchCoherent(*entry, vs_microcode_), memoized per VS load generation (a pure function of both).
  bool FetchCoherent(const masseffect::native::ShaderEntry* entry) {
    if (!identity_flat_) return masseffect::native::FetchCoherent(*entry, vs_microcode_);
    IdentityCell& cell = fc_cells_[IdentityCellOf(entry)];
    if (cell.candidate != entry || cell.generation != vs_identity_generation_) {
      cell = {entry, vs_identity_generation_, masseffect::native::FetchCoherent(*entry, vs_microcode_)};
    } else if (check_identity_ > 0) {
      --check_identity_;
      if (cell.result != masseffect::native::FetchCoherent(*entry, vs_microcode_)) {
        REXLOG_ERROR("[native] DIFFERENCE: flat fetch-coherence memo disagrees with the direct check (entry n{}); the old "
                     "path from now on", entry->number);
        identity_flat_ = false;
        return masseffect::native::FetchCoherent(*entry, vs_microcode_);
      }
    }
    return cell.result;
  }
  bool MatchesPixelShaderIdentity(const masseffect::native::ShaderEntry* candidate) {
    if (!candidate) return false;
    if (!identity_flat_) return ComputePixelIdentity(candidate);
    IdentityCell& cell = ps_cells_[IdentityCellOf(candidate)];
    if (cell.candidate != candidate || cell.generation != ps_identity_generation_) {
      cell = {candidate, ps_identity_generation_, ComputePixelIdentity(candidate)};
    } else if (check_identity_ > 0) {
      --check_identity_;
      if (cell.result != ComputePixelIdentity(candidate)) {
        REXLOG_ERROR("[native] DIFFERENCE: flat PS identity memo disagrees with the direct compare (entry n{}); the "
                     "old path from now on", candidate->number);
        identity_flat_ = false;
        return ComputePixelIdentity(candidate);
      }
    }
    return cell.result;
  }

  bool MatchesVertexShaderIdentity(const masseffect::native::ShaderEntry* candidate) {
    if (!candidate) return false;
    if (identity_flat_) {
      IdentityCell& cell = vs_cells_[IdentityCellOf(candidate)];
      if (cell.candidate != candidate || cell.generation != vs_identity_generation_) {
        cell = {candidate, vs_identity_generation_, ComputeVertexIdentity(candidate)};
      } else if (check_identity_ > 0) {
        --check_identity_;
        if (cell.result != ComputeVertexIdentity(candidate)) {
          REXLOG_ERROR("[native] DIFFERENCE: flat VS identity memo disagrees with the direct compare (entry n{}); the "
                       "old path from now on", candidate->number);
          identity_flat_ = false;
          return ComputeVertexIdentity(candidate);
        }
      }
      return cell.result;
    }
    auto& cached = vs_identity_cache_[candidate];
    if (cached.first != vs_identity_generation_) {
      cached.first = vs_identity_generation_;
      cached.second = VertexShaderIdentityMatches(
          {ShaderIdentityStage::Vertex, vs_microcode_},
          {candidate->vertices ? ShaderIdentityStage::Vertex : ShaderIdentityStage::Pixel,
           candidate->microcode}, candidate->elements,
          [](const auto& element) { return element.instruction; });
    }
    return cached.second;
  }

  // Once per (loaded program, candidate): every differing word between the loaded VS and the library candidate
  // (same size only), so an unresolved shader can be understood or added to the library without a debugger.
  void LogVertexWordDiff(const masseffect::native::ShaderEntry& candidate) {
    if (!candidate.vertices || vs_microcode_.empty() || vs_microcode_.size() != candidate.microcode.size() ||
        vs_diff_logged_.size() >= 40) return;
    const uint64_t key = current_vs_hash_ ^ (uint64_t(candidate.number) * 0x9E3779B97F4A7C15ull);
    if (!vs_diff_logged_.insert(key).second) return;
    std::string diffs;
    size_t count = 0;
    for (size_t w = 0; w < vs_microcode_.size(); ++w) {
      if (vs_microcode_[w] == candidate.microcode[w]) continue;
      if (++count <= 24)
        diffs += fmt::format(" [w{} i{}.{} {:08X}>{:08X}]", w, w / 3, w % 3, vs_microcode_[w], candidate.microcode[w]);
    }
    std::string declared;
    for (const auto& element : candidate.elements) declared += fmt::format(" {}", element.instruction);
    REXLOG_WARN("[native] VS word diff loaded_hash={:016X} words={} candidate n{} differing_words={} "
                "(loaded>library, first 24) declared_fetch_instr:{} diffs:{}",
                current_vs_hash_, vs_microcode_.size(), candidate.number, count, declared, diffs);
  }
  std::unordered_set<uint64_t> vs_diff_logged_;

  bool AcceptVertexShaderIdentity(const masseffect::native::ShaderEntry* candidate,
                                  const char* provenance) {
    if (!candidate) return false;
    if (MatchesVertexShaderIdentity(candidate)) return true;
    DumpLoadedVertexVariant(*candidate);
    LogVertexWordDiff(*candidate);
    const uint64_t mismatch = ++vs_identity_mismatches_;
    if (mismatch <= 32) {
      // Diagnostic only: mirror the predicate's rejection order without relaxing
      // any field. Loaded words are raw; the library words are already masked.
      constexpr uint32_t masks[3]{0x0007FFFFu, 0x80000FFFu, 0x80000000u};
      const char* reason = "unknown";
      size_t failed_word = SIZE_MAX;
      uint32_t failed_mask = UINT32_MAX;
      if (!candidate->vertices) reason = "wrong-stage";
      else if (vs_microcode_.empty()) reason = "empty-loaded";
      else if (vs_microcode_.size() != candidate->microcode.size()) reason = "word-count";
      else {
        bool failed = false;
        for (const auto& element : candidate->elements) {
          const size_t start = size_t(element.instruction) * 3;
          if (start + 3 > candidate->microcode.size()) {
            reason = "declared-fetch-out-of-bounds";
            failed_word = start;
            failed = true;
            break;
          }
          if ((candidate->microcode[start] & 0x1Fu) != 0u) {
            reason = "declared-fetch-opcode";
            failed_word = start;
            failed = true;
            break;
          }
          for (size_t lane = 0; lane < 3; ++lane) {
            if ((candidate->microcode[start + lane] & masks[lane]) !=
                candidate->microcode[start + lane]) {
              reason = "candidate-not-normalized";
              failed_word = start + lane;
              failed_mask = masks[lane];
              failed = true;
              break;
            }
          }
          if (failed) break;
          if (!VertexFetchSwizzleRepresentable(candidate->microcode[start + 1] & 0xFFFu,
                                               vs_microcode_[start + 1] & 0xFFFu)) {
            reason = "unrepresentable-fetch-swizzle";
            failed_word = start + 1;
            failed_mask = 0xFFFu;
            failed = true;
            break;
          }
        }
        if (!failed) {
          for (size_t word = 0; word < vs_microcode_.size(); ++word) {
            uint32_t mask = UINT32_MAX;
            for (const auto& element : candidate->elements) {
              const size_t start = size_t(element.instruction) * 3;
              if (word >= start && word - start < 3) {
                mask = word - start == 1 ? 0x80000000u : masks[word - start];
                break;
              }
            }
            if ((vs_microcode_[word] & mask) != (candidate->microcode[word] & mask)) {
              reason = mask == UINT32_MAX ? "non-fetch-word" : "masked-fetch-word";
              failed_word = word;
              failed_mask = mask;
              break;
            }
          }
        }
      }
      const uint32_t raw = failed_word < vs_microcode_.size() ? vs_microcode_[failed_word] : 0;
      const uint32_t selected = failed_word < candidate->microcode.size()
                                    ? candidate->microcode[failed_word] : 0;
      std::string declared;
      for (const auto& element : candidate->elements)
        declared += fmt::format(" {}", element.instruction);
      REXLOG_WARN("[native] VS identity detail mismatch={} reason={} word={} instruction={} lane={} "
                  "loaded={:08X} selected={:08X} mask={:08X} masked_loaded={:08X} declared_triples={}",
                  mismatch, reason, failed_word, failed_word / 3, failed_word % 3,
                  raw, selected, failed_mask, raw & failed_mask, declared);
    }
    if (mismatch <= 32 || (mismatch & 255) == 0) {
      const uint64_t selected_hash = XXH3_64bits(candidate->microcode.data(),
                                                candidate->microcode.size() * sizeof(uint32_t));
      REXLOG_WARN("[native] VS identity mismatch {} draw={} packet={:08X} "
                  "provenance={} loaded_host_hash={:016X} loaded_words={} selected_n={} "
                  "selected_stage={} selected_normalized_host_hash={:016X} selected_words={} "
                  "selected_container={:016X}",
                  mismatch, draws_, packet_address_, provenance,
                  current_vs_hash_, vs_microcode_.size(), candidate->number,
                  candidate->vertices ? "VS" : "PS", selected_hash, candidate->microcode.size(),
                  candidate->shader ? candidate->shader->fingerprint : 0);
    }
    return false;
  }

  const masseffect::native::ShaderEntry* PreferVertexShaderCandidate(
      const masseffect::native::ShaderEntry* current,
      const masseffect::native::ShaderEntry* candidate, const char* provenance) {
    // Keep diagnostics/variant discovery independent of the rollout policy.
    if (!candidate || !AcceptVertexShaderIdentity(candidate, provenance)) return current;
    return ShouldReplaceVertexCandidate(current != nullptr, MatchesVertexShaderIdentity(current),
                                        true, MatchesVertexShaderIdentity(candidate),
                                        false)
               ? candidate : current;
  }

  bool AcceptPixelShaderIdentity(const masseffect::native::ShaderEntry* candidate,
                                  const char* provenance) {
    if (!candidate) return false;
    const bool matches = MatchesPixelShaderIdentity(candidate);
    if (matches) return true;
    const uint64_t mismatch = ++ps_identity_mismatches_;
    if (mismatch <= 32 || (mismatch & 255) == 0) {
      const uint64_t selected_hash = XXH3_64bits(candidate->microcode.data(),
                                                candidate->microcode.size() * sizeof(uint32_t));
      REXLOG_WARN("[native] PS identity mismatch {} draw={} packet={:08X} "
                  "provenance={} loaded_host_hash={:016X} loaded_words={} selected_n={} "
                  "selected_stage={} selected_host_hash={:016X} selected_words={} "
                  "selected_container={:016X}",
                  mismatch, draws_, packet_address_, provenance,
                  current_ps_hash_, ps_microcode_.size(), candidate->number,
                  candidate->vertices ? "VS" : "PS", selected_hash, candidate->microcode.size(),
                  candidate->shader ? candidate->shader->fingerprint : 0);
    }
    return false;
  }

  // masseffect_native_raw_microcode: IdentifyShader's "same program reloaded from the same address" test on the raw
  // guest words. True = nothing to do (the stage keeps its entry, hashes and generations).
  bool ReloadsSameMicrocode(uint32_t type, const uint8_t* src, uint32_t size) {
    if (!REXCVAR_GET(masseffect_native_repeated_identity)) return false;
    const auto& raw = type == 1 ? raw_ps_ : raw_vs_;
    const auto* previous_entry = type == 1 ? ps_ : vs_;
    if (!previous_entry || !(type == 1 ? raw_ps_valid_ : raw_vs_valid_) || raw.size() != size ||
        (type == 1 ? last_ps_load_address_ : last_vs_load_address_) != im_load_address_ ||
        std::memcmp(raw.data(), src, size_t(size) * 4) != 0) {
      return false;
    }
    if (check_raw_ > 0) {
      --check_raw_;
      const auto& swapped = type == 1 ? ps_microcode_ : vs_microcode_;
      bool equal = swapped.size() == size;
      for (uint32_t i = 0; equal && i < size; ++i)
        equal = swapped[i] == rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
      if (!equal) {
        REXLOG_ERROR("[native] DIFFERENCE: raw microcode compare said 'same program' ({} words, type {}, address "
                     "{:08X}) but the swapped words differ; the old path from now on", size, type, im_load_address_);
        raw_microcode_ = false;
        return false;
      }
    }
    ++shader_loads_reused_;
    return true;
  }

  void IdentifyShader(uint32_t type) {
    if (type > 1) return;
    // The same program reloaded from the same address (D3D re-sends it for most draws): keep the identified
    // entry, the hashes and the generations, instead of copying, hashing twice and matching again.
    if (REXCVAR_GET(masseffect_native_repeated_identity)) {
      const auto& previous = type == 1 ? ps_microcode_ : vs_microcode_;
      const auto* previous_entry = type == 1 ? ps_ : vs_;
      if (previous_entry && previous.size() == microcode_.size() &&
          (type == 1 ? last_ps_load_address_ : last_vs_load_address_) == im_load_address_ &&
          std::memcmp(previous.data(), microcode_.data(), microcode_.size() * sizeof(uint32_t)) == 0) {
        ++shader_loads_reused_;
        return;
      }
    }
    (type == 1 ? last_ps_load_address_ : last_vs_load_address_) = im_load_address_;
    // Memo by content (masseffect_native_identity_memo): the same program loaded again after others, from any
    // address, gets the entry identified before; the draw-time guard still checks it against this microcode.
    const uint64_t memo_hash = XXH3_64bits(microcode_.data(), microcode_.size() * sizeof(uint32_t));
    const uint64_t memo_key = memo_hash ^ (uint64_t(type) << 63) ^ (uint64_t(microcode_.size()) << 40);
    if (REXCVAR_GET(masseffect_native_identity_memo)) {
      const auto it = identity_memo_.find(memo_key);
      if (it != identity_memo_.end()) {
        if (type == 1) {
          ps_microcode_ = microcode_;
          ++ps_identity_generation_;
          current_ps_hash_ = memo_hash;
          ps_ = it->second;
        } else {
          vs_microcode_ = microcode_;
          current_vs_hash_ = memo_hash;
          ++vs_identity_generation_;
          vs_ = it->second;
          gen_vs_ = ++gen_microcode_;
        }
        ++shaders_identified_;
        ++identity_memo_hits_;
        return;
      }
    }
    if (type == 1) {
      ps_microcode_ = microcode_;
      ++ps_identity_generation_;
      current_ps_hash_ = memo_hash;
    } else {
      // Identity checks below must see THIS IM_LOAD, not the previous VS load.
      vs_microcode_ = microcode_;
      current_vs_hash_ = memo_hash;
      ++vs_identity_generation_;
    }
    // A VS address identifies the object, not necessarily the variant currently in the ring: D3D
    // rewrites vertex fetches for each declaration. Prefer the actual loaded microcode (exact, then
    // the library's masked-fetch matcher), and accept the address entry only if it is compatible.
    const masseffect::native::ShaderEntry* by_address =
        im_load_address_ ? ByAddress(im_load_address_) : nullptr;
    const masseffect::native::ShaderEntry* entry = nullptr;
    if (type == 0) {
      const masseffect::native::ShaderEntry* observed = nullptr;
      entry = ByCode(microcode_, true, &observed);
      if (observed && !AcceptVertexShaderIdentity(observed, "identify-code")) entry = nullptr;
      if (entry && (entry->microcode.size() != microcode_.size() ||
                    !masseffect::native::FetchCoherent(*entry, microcode_))) {
        entry = nullptr;
      }
      // Guard OFF is permission for a last-resort legacy fallback, not for a
      // stale fast/address candidate to hide a proven current program.
      if (!MatchesVertexShaderIdentity(entry) && shaders_.loaded()) {
        const auto* matched = shaders_.Identify(true, microcode_);
        entry = PreferVertexShaderCandidate(entry, matched, "identify-library");
      }
      if (by_address && (!entry || (!MatchesVertexShaderIdentity(entry) &&
                                   MatchesVertexShaderIdentity(by_address))) &&
          by_address->microcode.size() == microcode_.size() &&
          masseffect::native::FetchCoherent(*by_address, microcode_)) {
        entry = PreferVertexShaderCandidate(entry, by_address, "identify-address");
      }
      if (entry == by_address && entry) ++shaders_by_address_;
      else if (entry) ++shaders_by_code_;
    } else {
      if (by_address &&
          AcceptPixelShaderIdentity(by_address, "identify-address")) {
        entry = by_address;
        ++shaders_by_address_;
      }
      if (!entry) {
        const masseffect::native::ShaderEntry* observed = nullptr;
        const auto* code_entry = ByCode(microcode_, false, &observed);
        const bool accepted = observed && AcceptPixelShaderIdentity(observed, "identify-code");
        if (code_entry && accepted) {
          entry = code_entry;
          ++shaders_by_code_;
        }
      }
      if (!entry && shaders_.loaded()) {
        const auto* matched = shaders_.Identify(false, microcode_);
        if (matched && AcceptPixelShaderIdentity(matched, "identify-library")) entry = matched;
      }
    }
    (type == 0 ? vs_ : ps_) = entry;
    (type == 0 ? current_vs_hash_ : current_ps_hash_) = memo_hash;
    if (entry && REXCVAR_GET(masseffect_native_identity_memo)) {
      if (identity_memo_.size() > 8192) identity_memo_.clear();
      identity_memo_.emplace(memo_key, entry);
    }
    if (type == 0) {
      gen_vs_ = ++gen_microcode_;
    }
    ++(entry ? shaders_identified_ : shaders_unidentified_);
    if (!entry) {
      ++(im_load_address_ ? (type == 0 ? miss_vs_addr_ : miss_ps_addr_) : miss_immediate_);
      DumpRawMicrocode(type == 0, im_load_address_, microcode_);
      if (!im_load_address_ && shaders_.loaded()) {
        DumpMissingImmediate(shaders_, type == 0, microcode_);
      }
    }
    if (!entry && logged_misses_ < 12) {  // diagnostic: what the ring loads that the library lacks
      ++logged_misses_;
      std::string keys;
      {
        std::lock_guard<std::mutex> lock(g_by_address_mutex);
        for (const auto& [k, v] : g_by_address) {
          if ((k >> 12) == (im_load_address_ >> 12)) keys += fmt::format(" {:08X}", k);
        }
      }
      REXLOG_INFO("[native] unidentified {} IM_LOAD at {:08X} (keys in page:{}): {} words, {:08X} {:08X} {:08X} {:08X}",
                  type == 0 ? "VS" : "PS", im_load_address_, keys, microcode_.size(), microcode_.size() > 0 ? microcode_[0] : 0,
                  microcode_.size() > 1 ? microcode_[1] : 0, microcode_.size() > 2 ? microcode_[2] : 0,
                  microcode_.size() > 3 ? microcode_[3] : 0);
    }
  }

  // Opt-in discovery only, not permission to select a mismatching program. Keep
  // the SAME-size template's complete physical payload, including DEF literals;
  // changing only its instruction bytes makes offline translation auditable.
  void DumpLoadedVertexVariant(const masseffect::native::ShaderEntry& candidate) {
    static const char* const folder = std::getenv("MASSEFFECT_VERTEX_VARIANTS");
    if (!folder || !*folder || !candidate.vertices || !candidate.shader || vs_microcode_.empty() ||
        vs_microcode_.size() != candidate.microcode.size() || dumped_vertex_variants_.size() >= 64) return;
    const auto& original = candidate.shader->original;
    if (original.size() < 28 || original.size() > 8u * 1024 * 1024) return;
    const uint32_t flags = rex::memory::load_and_swap<uint32_t>(original.data());
    const uint32_t virtual_bytes = rex::memory::load_and_swap<uint32_t>(original.data() + 4);
    const uint32_t physical_bytes = rex::memory::load_and_swap<uint32_t>(original.data() + 8);
    const uint32_t header = rex::memory::load_and_swap<uint32_t>(original.data() + 24);
    if ((flags & 0xFFFFFF00u) != 0x102A1100u || !(flags & 1u) || virtual_bytes > original.size() ||
        physical_bytes > original.size() - virtual_bytes || header > virtual_bytes ||
        virtual_bytes - header < 8) return;
    const uint32_t code_offset = rex::memory::load_and_swap<uint32_t>(original.data() + header);
    const uint32_t code_bytes = rex::memory::load_and_swap<uint32_t>(original.data() + header + 4);
    if (code_bytes != vs_microcode_.size() * 4 || code_offset > physical_bytes ||
        code_bytes > physical_bytes - code_offset) return;
    const uint64_t loaded_hash = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * 4);
    const uint64_t key = XXH3_64bits_withSeed(original.data(), original.size(), loaded_hash);
    if (!dumped_vertex_variants_.insert(key).second) return;
    std::vector<uint8_t> container(original);
    for (size_t i = 0; i < vs_microcode_.size(); ++i)
      rex::memory::store_and_swap<uint32_t>(container.data() + virtual_bytes + code_offset + i * 4,
                                          vs_microcode_[i]);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) { REXLOG_WARN("[native] vertex variant discovery directory failed: {}", ec.message()); return; }
    const auto path = std::filesystem::path(folder) / fmt::format("vs_from_n{}_{:016x}.bin", candidate.number, key);
    const auto pending_path = std::filesystem::path(path.string() + ".pending");
    bool good = false;
    if (auto* file = std::fopen(pending_path.string().c_str(), "wbx")) {
      good = std::fwrite(container.data(), 1, container.size(), file) == container.size();
      if (std::fclose(file) != 0) good = false;
    }
    if (good) {
      std::filesystem::create_hard_link(pending_path, path, ec);
      good = !ec;
    }
    REXLOG_INFO("[native] vertex variant discovery written={} template_n={} loaded_host_hash={:016X} "
                "words={} (complete template retained, NOT verified compatible): {}",
                good, candidate.number, loaded_hash, vs_microcode_.size(), path.string());
  }

  // With MASSEFFECT_SHADER_MISSING=<folder>: the container of a Direct3D vertex shader as the ring loaded
  // it (rewritten for the vertex declaration), made of the object's container header and the ring's
  // microcode, once per microcode. shaders/tools/build_shader_spirv.sh adds it to the library.
  void DumpRewrittenVertexShader(const masseffect::native::ShaderEntry& object_entry) {
    static const char* const folder = std::getenv("MASSEFFECT_SHADER_MISSING");
    if (!folder || !*folder || !object_entry.shader || vs_microcode_.empty()) return;
    const uint64_t code = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
    if (!dumped_rewritten_.insert(code).second) return;
    const auto& o = object_entry.shader->original;
    if (o.size() < 28) return;
    const uint32_t vsize = rex::memory::load_and_swap<uint32_t>(o.data() + 4);
    const uint32_t sh = rex::memory::load_and_swap<uint32_t>(o.data() + 24);
    if (vsize > o.size() || sh + 8 > vsize) return;
    const uint32_t bytes = uint32_t(vs_microcode_.size() * 4);
    std::vector<uint8_t> container(o.begin(), o.begin() + vsize);
    rex::memory::store_and_swap<uint32_t>(container.data() + 8, bytes);   // physical size
    rex::memory::store_and_swap<uint32_t>(container.data() + sh, 0);      // microcode offset
    rex::memory::store_and_swap<uint32_t>(container.data() + sh + 4, bytes);
    container.resize(size_t(vsize) + bytes);
    for (size_t i = 0; i < vs_microcode_.size(); ++i) {
      rex::memory::store_and_swap<uint32_t>(container.data() + vsize + i * 4, vs_microcode_[i]);
    }
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const auto path = std::filesystem::path(folder) /
                      fmt::format("vs_{:016x}.bin", XXH3_64bits(container.data(), container.size()));
    if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
      std::fwrite(container.data(), 1, container.size(), f);
      std::fclose(f);
      REXLOG_INFO("[native] rewritten Direct3D vertex shader kept: {}", path.string());
    }
  }

  // The library entries of the VS and PS objects a record names (nullptr = unknown object).
  void LookupObjectEntries(uint32_t object_vs, uint32_t object_ps, const masseffect::native::ShaderEntry*& vs,
                           const masseffect::native::ShaderEntry*& ps) {
    vs = ps = nullptr;
    if (pairing_fast_ && LookupObject(object_vs, vs) && LookupObject(object_ps, ps)) {
      if (check_objects_ > 0) {
        --check_objects_;
        const masseffect::native::ShaderEntry* want_vs = nullptr;
        const masseffect::native::ShaderEntry* want_ps = nullptr;
        {
          std::lock_guard<std::mutex> lock(g_by_address_mutex);
          auto a = g_entry_of_object.find(object_vs);
          auto b = g_entry_of_object.find(object_ps);
          if (a != g_entry_of_object.end()) want_vs = a->second;
          if (b != g_entry_of_object.end()) want_ps = b->second;
        }
        if (want_vs != vs || want_ps != ps) {  // the game thread may have just added or replaced an entry: look again
          const masseffect::native::ShaderEntry* again_vs = nullptr;
          const masseffect::native::ShaderEntry* again_ps = nullptr;
          LookupObject(object_vs, again_vs);
          LookupObject(object_ps, again_ps);
          if (again_vs != want_vs || again_ps != want_ps) {
            REXLOG_ERROR("[native] DIFFERENCE: lock-free object table: VS {:08X} {} / {}, PS {:08X} {} / {} (table / "
                         "map); the old path from now on", object_vs, static_cast<const void*>(again_vs),
                         static_cast<const void*>(want_vs), object_ps, static_cast<const void*>(again_ps),
                         static_cast<const void*>(want_ps));
            pairing_fast_ = false;
          }
          vs = want_vs;
          ps = want_ps;
        }
      }
      return;
    }
    vs = ps = nullptr;
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    auto a = g_entry_of_object.find(object_vs);
    auto b = g_entry_of_object.find(object_ps);
    if (a != g_entry_of_object.end()) vs = a->second;
    if (b != g_entry_of_object.end()) ps = b->second;
  }

  // By packet address: the record whose command-buffer start is the closest one below this DRAW_INDX.
  bool PairByAddress(uint32_t type) {
    DrawRecord r;
    if (RecordTable()) {
      DrainRecords();  // the table belongs to the ring thread: no lock
      DrawRecordSlot* best = nullptr;
      if (pairing_fast_) {
        best = g_records.FindDense(packet_address_);
        if (check_pairing_ > 0) {
          --check_pairing_;
          DrawRecordSlot* old = g_records.FindScan(packet_address_);
          if (old != best) {
            REXLOG_ERROR("[native] DIFFERENCE: fast record lookup for packet {:08X} chose slot {} but the scan chose {}; "
                         "the old path from now on", packet_address_, best ? int64_t(g_records.IndexOf(best)) : -1,
                         old ? int64_t(g_records.IndexOf(old)) : -1);
            pairing_fast_ = false;
            g_record_fast = false;
            best = old;
          }
        }
      } else {
        best = g_records.FindScan(packet_address_);
      }
      // The map's rule: the nearest record at or below the packet, within 0x2000 and of the same type.
      if (!best || packet_address_ - best->start > 0x2000 || best->record.type != type) return false;
      r = std::move(best->record);
      g_records.Consume(best);
    } else {
      std::lock_guard<std::mutex> lock(g_draw_mutex);
      auto it = g_draw_by_address.upper_bound(packet_address_);
      if (it == g_draw_by_address.begin()) return false;
      --it;
      if (packet_address_ - it->first > 0x2000 || it->second.type != type) return false;
      r = it->second;
      g_draw_by_address.erase(it);
    }
    const masseffect::native::ShaderEntry* vs = nullptr;
    const masseffect::native::ShaderEntry* ps = nullptr;
    LookupObjectEntries(r.vs, r.ps, vs, ps);
    if (vs && !AcceptVertexShaderIdentity(vs, "pair-address-object")) vs = nullptr;
    if (vs && vs->binding_per_register) {
      // Direct3D rewrote this vertex shader for the vertex declaration: the variant the ring loaded is
      // its own library entry, found by microcode (vs_). Without it, keep a copy to extend the library.
      if (vs_ && vs_->binding_per_register) {
        vs = vs_;
      } else {
        DumpRewrittenVertexShader(*vs);
      }
    }
    // Audit the object even when the old register-linked override would hide a
    // stale entry. The guard protects every path, not just shader loading.
    if (ps && !AcceptPixelShaderIdentity(ps, "pair-address-object")) ps = nullptr;
    if (ps && ps->binding_per_register) {
      // Direct3D reuses its small internal PS objects and overwrites the same ring allocation with
      // different microcode. The object entry therefore describes only the first variant observed;
      // the IM_LOAD at this draw is authoritative, exactly as for rewritten register-linked VS.
      ps = ps_ && ps_->binding_per_register ? ps_ : nullptr;
    }
    // The IM_LOAD is the authoritative patched variant. A Draw* record may point at the
    // unpatched object, or at a different variant queued ahead of this indirect buffer.
    // Do not combine such an entry with the current ring microcode: ComputeEntry would
    // reject it with cause 20 (or derive the wrong vertex layout).
    if (vs && AcceptVertexShaderIdentity(vs, "pair-address-selected") &&
        vs->microcode.size() == vs_microcode_.size() &&
        FetchCoherent(vs)) {
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, vs, "pair-address-preference");
    }
    if (ps && AcceptPixelShaderIdentity(ps, "pair-address-selected")) draw_ps_ = ps;
    ++draws_paired_by_address_;
    if (!vs) {
      ++paired_no_vs_;
      if (missing_objects_.size() < 32 || missing_objects_.count(r.vs)) ++missing_objects_[r.vs];
    }
    if (!ps) {
      ++paired_no_ps_;
      if (missing_objects_.size() < 32 || missing_objects_.count(r.ps)) ++missing_objects_[r.ps];
    }
    return true;
  }

  // The first of up to 8 pending records with the same primitive type and
  // a matching count gives the VS and PS objects; otherwise the ring's own identification.
  void PairDraw() {
    draw_vs_ = vs_ && AcceptVertexShaderIdentity(vs_, "pair-initial-ring") ? vs_ : nullptr;
    draw_ps_ = ps_;
    if (PairByAddress(Register(kRegVgtDrawInitiator) & 0x3F)) return;
    // Predicated tiling replays the same command buffer once per EDRAM tile: a DRAW_INDX packet at an
    // address already paired in this frame reuses that pairing and consumes no record.
    if (auto it = paired_packets_.find(packet_address_);
        it != paired_packets_.end() && it->second.frame == swaps_) {
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, it->second.vs, "pair-replay");
      if (it->second.ps && AcceptPixelShaderIdentity(it->second.ps, "pair-replay"))
        draw_ps_ = it->second.ps;
      ++draws_replayed_;
      return;
    }
    if (!pairing_fast_) {  // nothing ever fills g_draw_queue (records go through the lock-free queue)
      std::lock_guard<std::mutex> lock(g_draw_mutex);
      while (pending_.size() < 4096 && !g_draw_queue.empty()) {
        pending_.push_back(g_draw_queue.front());
        g_draw_queue.pop_front();
      }
    }
    const uint32_t initiator = Register(kRegVgtDrawInitiator);
    const uint32_t type = initiator & 0x3F, count = initiator >> 16;
    const size_t n = std::min<size_t>(pending_.size(), 64);
    for (size_t i = 0; i < n; ++i) {
      const DrawRecord& r = pending_[i];
      if (r.type != type) continue;
      if (r.r5 != count && r.r6 != count && r.r7 != count && CountForPrimitives(type, r.r7) != count &&
          CountForPrimitives(type, r.r6) != count) {
        continue;
      }
      const masseffect::native::ShaderEntry* vs = nullptr;
      const masseffect::native::ShaderEntry* ps = nullptr;
      LookupObjectEntries(r.vs, r.ps, vs, ps);
      pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(i + 1));
      ++draws_paired_;
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, vs, "pair-queue");
      if (ps && AcceptPixelShaderIdentity(ps, "pair-queue")) draw_ps_ = ps;
      paired_packets_[packet_address_] = {swaps_, draw_vs_, ps};
      return;
    }
    ++draws_unpaired_;
    if (logged_unpaired_ < 30 && draws_ > 60000 && type != 1) {
      ++logged_unpaired_;
      std::string p;
      for (size_t i = 0; i < pending_.size() && i < 4; ++i) {
        p += fmt::format(" [t{} {} {} {}]", pending_[i].type, pending_[i].r5, pending_[i].r6, pending_[i].r7);
      }
      REXLOG_INFO("[native] unpaired ring draw: type {} count {} source {}; pending {}:{}", type, count,
                  (initiator >> 6) & 3, pending_.size(), p);
    }
  }

  // A proven rectangle (CPU-executed VS) that overwrites every covered pixel of its views:
  // no discard, full sample mask, depth written with ALWAYS and stencil off, or color fully masked
  // without blending. The EDRAM tiles fully inside need no transfer from their previous owner
  // (xenia's "spurious ownership transfer round trips"; here the shadow maps and the scene share EDRAM).
  void ProveRectangleList(masseffect::native::SubmissionDraw& request, uint32_t rects) {
    if (!request.ps) return;
    std::vector<uint32_t> regs(registers_);
    const uint32_t initiator = regs[kRegVgtDrawInitiator];
    const uint32_t offset = regs[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET];
    regs[kRegVgtDrawInitiator] = (initiator & 0xFFFFu) | (3u << 16);
    std::optional<std::array<int32_t, 4>> best, bounds;
    int64_t best_area = -1;
    uint32_t slots = 0;
    uint32_t max_y = 0;
    for (uint32_t k = 0; k < rects; ++k) {
      regs[rex::graphics::XE_GPU_REG_VGT_INDX_OFFSET] = offset + 3u * k;
      NativeDrawExtentEstimator::Diagnostics diagnostic;
      const auto estimate = draw_extent_estimator_->Estimate(regs, vs_microcode_, &diagnostic);
      if (!estimate) { ++multi_rect_rejects_[0]; return; }  // every rectangle must be proven
      max_y = std::max(max_y, *estimate);
      StencilClearOwnershipInput proof;
      proof.depth_control = Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL);
      proof.stencil_front = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK);
      proof.stencil_back = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF);
      proof.color_control = Register(rex::graphics::XE_GPU_REG_RB_COLORCONTROL);
      proof.su_sc_mode = Register(rex::graphics::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
      proof.vte_cntl = Register(rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL);
      const uint32_t tl = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
      const uint32_t br = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
      const uint32_t window_offset = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
      const auto sign15 = [](uint32_t v) { return int32_t(v & 0x7FFF) - int32_t((v & 0x4000) << 1); };
      const int32_t wx = (tl >> 31) ? 0 : sign15(window_offset);
      const int32_t wy = (tl >> 31) ? 0 : sign15(window_offset >> 16);
      const uint32_t screen_tl = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL);
      const uint32_t screen_br = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR);
      proof.scissor_x0 = uint32_t(std::max({0, int32_t(tl & 0x3FFF) + wx, sign15(screen_tl)}));
      proof.scissor_y0 = uint32_t(std::max({0, int32_t((tl >> 16) & 0x3FFF) + wy, sign15(screen_tl >> 16)}));
      proof.scissor_x1 = uint32_t(std::max(0, std::min(int32_t(br & 0x3FFF) + wx, sign15(screen_br))));
      proof.scissor_y1 = uint32_t(std::max(0, std::min(int32_t((br >> 16) & 0x3FFF) + wy, sign15(screen_br >> 16))));
      proof.post_vs_position = diagnostic.position;
      proof.xy_raster = diagnostic.xy_raster;
      proof.ps_no_discard = ProveStraightLinePixelNoKill(ps_microcode_) ||
          (REXCVAR_GET(masseffect_native_edram4_ps_no_kill) && ProvePixelNoKillAnywhere(ps_microcode_));
      proof.ps_no_depth_export = !(request.ps->outputs & 0x10);
      proof.sample_coverage_proven_full = (Register(rex::graphics::XE_GPU_REG_PA_SC_AA_MASK) & 0xFFFFu) == 0xFFFFu;
      masseffect::native::SubmissionDraw one = request;
      one.edram_overwrite_rect.reset();
      ProveFullOverwrite(one, proof);
      if (!one.edram_overwrite_rect) { ++multi_rect_rejects_[1]; return; }
      if (k && one.edram_overwrite_slots != slots) { ++multi_rect_rejects_[2]; return; }
      request.edram_overwrite_rects[k] = *one.edram_overwrite_rect;
      slots = one.edram_overwrite_slots;
      const auto& r = *one.edram_overwrite_rect;
      const int64_t area = int64_t(r[2] - r[0]) * (r[3] - r[1]);
      if (area > best_area) { best_area = area; best = r; }
      const auto& b = one.edram_bounds_rect ? *one.edram_bounds_rect : r;
      if (!bounds) bounds = b;
      else bounds = std::array<int32_t, 4>{std::min((*bounds)[0], b[0]), std::min((*bounds)[1], b[1]),
                                           std::max((*bounds)[2], b[2]), std::max((*bounds)[3], b[3])};
    }
    // Pixels the coverage rectangles exclude (partial pixels at the edges) are not overwritten, but they are
    // covered by the bounds only if covered at all: the proven rects are exactly the covered pixel sets.
    request.edram_used_height_estimate = max_y;
    request.edram_overwrite_rect = best;
    request.edram_overwrite_slots = slots;
    request.edram_bounds_rect = bounds;
    request.edram_overwrite_rect_count = rects;
    ++multi_rect_proven_;
  }
  uint64_t multi_rect_proven_ = 0;
  std::unordered_map<uint64_t, const masseffect::native::ShaderEntry*> identity_memo_;
  uint64_t identity_memo_hits_ = 0;
  uint64_t exact_edges_ = 0;
  uint32_t edges_traces_ = 0, alias_traces_ = 0, traces_depth_ = 0;
  std::array<uint64_t, 3> multi_rect_rejects_{};  // estimator, overwrite proof, slot mismatch

  void ProveFullOverwrite(masseffect::native::SubmissionDraw& request, const StencilClearOwnershipInput& proof) {
    if (!REXCVAR_GET(masseffect_native_edram4_overwrite) || !request.ps) return;
    if (!proof.ps_no_discard) { ++overwrite_rejects_[0]; return; }
    if (!proof.sample_coverage_proven_full) { ++overwrite_rejects_[1]; return; }
    const auto& pos = proof.post_vs_position;
    float x0 = pos[0][0], x1 = pos[0][0], y0 = pos[0][1], y1 = pos[0][1];
    for (const auto& v : pos) {
      x0 = std::min(x0, v[0]); x1 = std::max(x1, v[0]);
      y0 = std::min(y0, v[1]); y1 = std::max(y1, v[1]);
    }
    // OpenGL pixel centers (PA_SU_VTX_CNTL.pix_center = 1, at i + 0.5): the same coverage as D3D centers
    // with every position half a pixel lower. The rest of the proof uses the D3D convention.
    if (REXCVAR_GET(masseffect_native_edram4_center_ogl) &&
        (Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL) & 1)) {
      x0 -= 0.5f; x1 -= 0.5f; y0 -= 0.5f; y1 -= 0.5f;
    }
    // D3D9 pixel space: pixel i spans [i - 0.5, i + 0.5), so [-0.5, W - 0.5] covers pixels 0..W-1.
    // A small epsilon rejects rectangles that stop short of a pixel by rounding.
    constexpr float kEps = 1.0f / 64.0f;
    std::array<int32_t, 4> rect{int32_t(std::ceil(x0 + 0.5f - kEps)), int32_t(std::ceil(y0 + 0.5f - kEps)),
                                int32_t(std::floor(x1 + 0.5f + kEps)), int32_t(std::floor(y1 + 0.5f + kEps))};
    rect[0] = std::max<int32_t>(rect[0], int32_t(proof.scissor_x0));
    rect[1] = std::max<int32_t>(rect[1], int32_t(proof.scissor_y0));
    rect[2] = std::min<int32_t>(rect[2], int32_t(proof.scissor_x1));
    rect[3] = std::min<int32_t>(rect[3], int32_t(proof.scissor_y1));
    if (rect[2] <= rect[0] || rect[3] <= rect[1]) { ++overwrite_rejects_[2]; return; }
    uint32_t slots = 0;
    const uint32_t dc = proof.depth_control;
    const bool z_enable = dc & 2, z_write = dc & 4, stencil = dc & 1;
    // Stencil is fully replaced when every sample passes (func ALWAYS; depth func ALWAYS so zfail never
    // happens), the pass op is REPLACE and the write mask is 0xFF, on both faces (D3D Clear rectangles).
    const auto stencil_replaced = [&](uint32_t func, uint32_t zpass, uint32_t refmask) {
      // REPLACE (2) writes the reference, ZERO (1) writes 0: either way every bit gets a known value.
      return func == 7 && (zpass == 2 || zpass == 1) && ((refmask >> 16) & 0xFFu) == 0xFFu;
    };
    // An inert stencil (test ALWAYS, every op KEEP or write mask 0, both faces) neither kills nor writes: the
    // draw is a depth-only draw for the overwrite proof (the depth restore pass, Z ALWAYS + stencil ALWAYS/KEEP).
    const auto inert = [&](uint32_t func, uint32_t ops, uint32_t refmask) {
      return func == 7 && (ops == 0 || ((refmask >> 16) & 0xFFu) == 0);
    };
    const bool stencil_inert = stencil && REXCVAR_GET(masseffect_native_edram4_stencil_inert) &&
        inert((dc >> 8) & 7, (dc >> 11) & 0x1FF, proof.stencil_front) &&
        (!(dc & 0x80) || inert((dc >> 20) & 7, (dc >> 23) & 0x1FF, proof.stencil_back));
    bool stencil_ok = !stencil || stencil_inert;
    if (stencil && !stencil_inert) {
      stencil_ok = stencil_replaced((dc >> 8) & 7, (dc >> 14) & 7, proof.stencil_front) &&
                   (!(dc & 0x80) || stencil_replaced((dc >> 20) & 7, (dc >> 26) & 7, proof.stencil_back));
    }
    // Alpha test or alpha-to-coverage can kill samples: then neither depth nor stencil is fully written.
    const bool killable = ((proof.color_control >> 3) & 1) || ((proof.color_control >> 4) & 1);
    if (!killable && stencil && stencil_ok && !stencil_inert && (!z_enable || ((dc >> 4) & 7) == 7))
      request.edram_stencil_replace_rect = rect;  // every sample reaches the stencil pass op
    if (!killable && z_enable && z_write && ((dc >> 4) & 7) == 7 && stencil_ok) slots |= 1;
    else if ((dc & 6) == 6) ++overwrite_rejects_[3];
    const uint32_t edram_mode = Register(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 7;
    const bool alpha_test = (proof.color_control >> 3) & 1;
    if (edram_mode == uint32_t(xenos::EdramMode::kColorDepth) && !alpha_test) {
      const uint32_t mask = Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK);
      static constexpr uint32_t kBlend[4] = {rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0,
                                             rex::graphics::XE_GPU_REG_RB_BLENDCONTROL1,
                                             rex::graphics::XE_GPU_REG_RB_BLENDCONTROL2,
                                             rex::graphics::XE_GPU_REG_RB_BLENDCONTROL3};
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(request.ps->outputs & (1u << i))) continue;
        if (((mask >> (i * 4)) & 15u) == 15u && (Register(kBlend[i]) & 0x1FFF1FFFu) == 0x00010001u)
          slots |= 2u << i;
      }
    }
    {
      // Diagnostic: a clear whose depth and color 0 share the EDRAM base (D3D Clear of both through the 4x alias).
      const uint32_t di = Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO), ci = Register(rex::graphics::XE_GPU_REG_RB_COLOR_INFO);
      if ((di & 0xFFF) == (ci & 0xFFF) && alias_traces_ < 12) {
        ++alias_traces_;
        REXLOG_INFO("[native] aliased depth/color clear proof: slots {:X} mode {} dc {:08X} colorctl {:08X} mask {:08X} "
                    "blend0 {:08X} ps outputs {:X} depthinfo {:08X} colorinfo {:08X} rect {},{}-{},{} killable {}",
                    slots, edram_mode, dc, proof.color_control, Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK),
                    Register(rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0), request.ps->outputs, di, ci,
                    rect[0], rect[1], rect[2], rect[3], killable);
      }
    }
    if (!slots) { ++overwrite_rejects_[4]; return; }
    ++overwrite_rejects_[5];
    // Area bound for mode 4: rounded outward and padded by a pixel, so it can only over-cover (the
    // overwrite rect above is rounded inward on purpose).
    // Edges exactly on pixel boundaries (k - 0.5 with D3D pixel centers) touch no sample of the neighbouring
    // pixel at 1x or 4x (samples at least 0.25 inside), so the inward rect is also the exact bound.
    const auto in_edge = [](float v) {
      const float b = v + 0.5f;
      return std::abs(b - std::round(b)) < 1.0f / 1024.0f;
    };
    const uint32_t vte_xy = proof.vte_cntl;
    const bool w_one = proof.xy_raster || (vte_xy & 0x100) ||
                       (pos[0][3] == 1.0f && pos[1][3] == 1.0f && pos[2][3] == 1.0f);
    const bool exact = REXCVAR_GET(masseffect_native_edram4_exact_edges) && w_one &&
        (!(Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL) & 1) || REXCVAR_GET(masseffect_native_edram4_center_ogl)) &&
        (!(proof.su_sc_mode & (1u << 16)) || !Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET)) &&
        in_edge(x0) && in_edge(x1) && in_edge(y0) && in_edge(y1);
    if (exact) ++exact_edges_;
    else if (REXCVAR_GET(masseffect_native_edram4_exact_edges) && edges_traces_ < 24) {
      ++edges_traces_;
      REXLOG_INFO("[native] edges not exact: x {}..{} y {}..{} w {} {} {} vte {:08X} vtx_cntl {:08X} su_sc {:08X} "
                  "rect {},{}-{},{} scissor {},{}-{},{}", x0, x1, y0, y1, pos[0][3], pos[1][3], pos[2][3], vte_xy,
                  Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL), proof.su_sc_mode, rect[0], rect[1], rect[2],
                  rect[3], proof.scissor_x0, proof.scissor_y0, proof.scissor_x1, proof.scissor_y1);
    }
    request.edram_bounds_rect = exact ? rect : std::array<int32_t, 4>{
        std::max<int32_t>(int32_t(std::floor(x0 + 0.5f)) - 1, int32_t(proof.scissor_x0)),
        std::max<int32_t>(int32_t(std::floor(y0 + 0.5f)) - 1, int32_t(proof.scissor_y0)),
        std::min<int32_t>(int32_t(std::ceil(x1 + 0.5f)) + 1, int32_t(proof.scissor_x1)),
        std::min<int32_t>(int32_t(std::ceil(y1 + 0.5f)) + 1, int32_t(proof.scissor_y1))};
    request.edram_overwrite_rect = rect;
    request.edram_overwrite_slots = slots;
    // Constant depth: the same post-viewport Z at all three corners, no polygon offset, no PS depth
    // export. Lets mode 4 apply the rectangle as a depth clear in the current owner's view.
    if ((slots & 1) && proof.ps_no_depth_export && !(proof.su_sc_mode & 0x3800u)) {
      const uint32_t vte = proof.vte_cntl;
      const float zs = (vte & 0x10) ? std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE))
                                    : 1.0f;
      const float zo = (vte & 0x20) ? std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET))
                                    : 0.0f;
      std::optional<float> z;
      bool constant = true;
      for (const auto& v : pos) {
        float zf = v[2];
        if (!(vte & 0x200)) zf = (vte & 0x400) ? zf / v[3] : zf * v[3];
        zf = zf * zs + zo;
        // A hair below 0 (-3.7e-9 in ME's clears) stores the all-zero depth field in both encodings (unorm24
        // clamps, float24 maps z <= 0 to 0).
        if (zf < 0.0f && zf > -1.0e-6f) zf = 0.0f;
        if (!std::isfinite(zf) || zf < 0.0f || zf > 1.0f || (z && *z != zf)) { constant = false; break; }
        z = zf;
      }
      if (constant && z) request.edram_overwrite_depth = z;
    }
    if ((slots & 1) && !request.edram_overwrite_depth && traces_depth_ < 16) {
      ++traces_depth_;
      REXLOG_INFO("[native] constant depth not proven: depthinfo {:08X} ps_no_depth_export {} su_sc {:08X} vte {:08X} "
                  "z {} {} {} w {} {} {} zscale {} zoffset {}", Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                  proof.ps_no_depth_export, proof.su_sc_mode, proof.vte_cntl, pos[0][2], pos[1][2], pos[2][2],
                  pos[0][3], pos[1][3], pos[2][3],
                  std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE)),
                  std::bit_cast<float>(Register(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET)));
    }
  }

  std::array<uint64_t, 6> overwrite_rejects_{};  // discard, sample mask, empty rect, depth/stencil, no slot, ok
  void Draw() {
    if (!MatchesVertexShaderIdentity(draw_vs_) && MatchesVertexShaderIdentity(vs_))
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, vs_, "draw-final-ring-preference");
    if (draw_vs_ && !AcceptVertexShaderIdentity(draw_vs_, "draw-final")) {
      // No unchecked fallback: the last loaded variant is the only authority.
      draw_vs_ = vs_ && AcceptVertexShaderIdentity(vs_, "draw-final-ring") ? vs_ : nullptr;
      if (!draw_vs_) { ++draws_unidentified_; return; }
    }
    if (!EnsureTargets()) return;
    // Candidate warnings include discarded object/queue alternatives. Count
    // the actual final selection separately, without changing rollout policy.
    if (!draw_vs_) {
      ++vs_final_missing_;
    } else if (MatchesVertexShaderIdentity(draw_vs_)) {
      ++vs_final_proven_;
    } else {
      const uint64_t unproven = ++vs_final_unproven_;
      if (unproven <= 8 || (unproven & 255) == 0) {
        REXLOG_WARN("[native] FINAL VS unproven {} draw={} loaded_host_hash={:016X} "
                    "loaded_words={} selected_n={} selected_container={:016X}",
                    unproven, draws_, current_vs_hash_, vs_microcode_.size(),
                    draw_vs_->number, draw_vs_->shader ? draw_vs_->shader->fingerprint : 0);
      }
    }
    masseffect::native::SubmissionDraw request;
    request.registers = registers_.data();
    request.vs = draw_vs_;
    request.ps = draw_ps_;
    request.vs_microcode = vs_microcode_;
    request.ps_microcode = ps_microcode_;
    request.generation_vs = gen_vs_;
    request.generation_constants_vs = gen_constants_vs_;
    request.generation_constants_ps = gen_constants_ps_;
    request.generation_fetch = gen_fetch_;
    request.generation_framing = gen_viewport_;
    if (memory_ &&
        ((Register(rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL) & 0x10000u) ||
         REXCVAR_GET(masseffect_native_edram4_clip_inside)) &&
        MatchesVertexShaderIdentity(draw_vs_)) {
      const uint32_t initiator = Register(kRegVgtDrawInitiator);
      // The adapter performs the full admission proof. This outer filter avoids
      // copying/analyzing unrelated normal draws when the opt-in is enabled.
      const uint32_t rect_vertices = initiator >> 16;
      if ((initiator & 63u) == uint32_t(xenos::PrimitiveType::kRectangleList) &&
          ((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kAutoIndex) &&
          rect_vertices > 3u && rect_vertices % 3u == 0 && rect_vertices <= 24u &&
          REXCVAR_GET(masseffect_native_edram4_several_rect)) {
        // Rect lists of several rectangles (D3D Clear of two regions): prove each one with the same
        // single-rectangle estimator on a register copy whose index offset selects that rectangle.
        if (!draw_extent_estimator_) { draw_extent_estimator_ = std::make_unique<NativeDrawExtentEstimator>(*memory_); draw_extent_estimator_->AllowClipInside(REXCVAR_GET(masseffect_native_edram4_clip_inside)); draw_extent_estimator_->AllowQuadTriangles(REXCVAR_GET(masseffect_native_edram4_quad_triangles)); draw_extent_estimator_->AllowAlu(REXCVAR_GET(masseffect_native_edram4_vs_alu)); draw_extent_estimator_->AllowPackedFormats(REXCVAR_GET(masseffect_native_extent_packed_formats)); }
        ProveRectangleList(request, rect_vertices / 3u);
      } else if ((((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kAutoIndex) ||
                  (((initiator >> 6) & 3u) == uint32_t(xenos::SourceSelect::kDMA) &&
                   (initiator & 63u) != uint32_t(xenos::PrimitiveType::kRectangleList))) &&
          (((initiator & 63u) == uint32_t(xenos::PrimitiveType::kRectangleList) && (initiator >> 16) == 3u) ||
           (REXCVAR_GET(masseffect_native_edram4_quad_triangles) &&
            (((initiator & 63u) == uint32_t(xenos::PrimitiveType::kTriangleList) && (initiator >> 16) == 6u) ||
             ((initiator & 63u) == uint32_t(xenos::PrimitiveType::kTriangleStrip) && (initiator >> 16) == 4u))))) {
        if (!draw_extent_estimator_) { draw_extent_estimator_ = std::make_unique<NativeDrawExtentEstimator>(*memory_); draw_extent_estimator_->AllowClipInside(REXCVAR_GET(masseffect_native_edram4_clip_inside)); draw_extent_estimator_->AllowQuadTriangles(REXCVAR_GET(masseffect_native_edram4_quad_triangles)); draw_extent_estimator_->AllowAlu(REXCVAR_GET(masseffect_native_edram4_vs_alu)); draw_extent_estimator_->AllowPackedFormats(REXCVAR_GET(masseffect_native_extent_packed_formats)); }
        NativeDrawExtentEstimator::Diagnostics diagnostic;
        request.edram_used_height_estimate = draw_extent_estimator_->Estimate(registers_, vs_microcode_, &diagnostic);
        if (request.edram_used_height_estimate && request.ps) {
          StencilClearOwnershipInput proof;
          proof.depth_control = Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL);
          proof.stencil_front = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK);
          proof.stencil_back = Register(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF);
          proof.color_control = Register(rex::graphics::XE_GPU_REG_RB_COLORCONTROL);
          proof.su_sc_mode = Register(rex::graphics::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
          proof.vtx_cntl = Register(rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL);
          proof.vte_cntl = Register(rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL);
          proof.window_offset = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
          proof.clip_cntl = Register(rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL);
          const uint32_t surface = Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
          proof.msaa_enum = (surface >> 16) & 3;
          proof.physical_pitch_tiles = (((surface & 0x3FFF) << uint32_t(proof.msaa_enum == 2)) + 79) / 80;
          const uint32_t tl = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
          const uint32_t br = Register(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
          const auto sign15 = [](uint32_t v) { return int32_t(v & 0x7FFF) - int32_t((v & 0x4000) << 1); };
          const int32_t wx = (tl >> 31) ? 0 : sign15(proof.window_offset);
          const int32_t wy = (tl >> 31) ? 0 : sign15(proof.window_offset >> 16);
          const uint32_t screen_tl = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL);
          const uint32_t screen_br = Register(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR);
          proof.scissor_x0 = uint32_t(std::max({0, int32_t(tl & 0x3FFF) + wx, sign15(screen_tl)}));
          proof.scissor_y0 = uint32_t(std::max({0, int32_t((tl >> 16) & 0x3FFF) + wy, sign15(screen_tl >> 16)}));
          proof.scissor_x1 = uint32_t(std::max(0, std::min(int32_t(br & 0x3FFF) + wx, sign15(screen_br))));
          proof.scissor_y1 = uint32_t(std::max(0, std::min(int32_t((br >> 16) & 0x3FFF) + wy, sign15(screen_br >> 16))));
          proof.post_vs_position = diagnostic.position;
      proof.xy_raster = diagnostic.xy_raster;
          proof.rectangle_proven = true;  // Successful bounded adapter proof, not bbox-only.
          proof.ps_identity_proven = MatchesPixelShaderIdentity(request.ps);
          // Counting SPIR-V OpKill is not proof: an optimizer may merge guest
          // and alpha-test kills. Inspect every admitted loaded ALU instead.
          proof.ps_no_discard = ProveStraightLinePixelNoKill(ps_microcode_) ||
          (REXCVAR_GET(masseffect_native_edram4_ps_no_kill) && ProvePixelNoKillAnywhere(ps_microcode_));
          proof.ps_no_depth_export = !(request.ps->outputs & 0x10);
          // Admit only the all-enabled 16-bit AA mask, never an unknown mask.
          // The native pipeline has no additional programmable sample mask.
          proof.sample_coverage_proven_full = (Register(rex::graphics::XE_GPU_REG_PA_SC_AA_MASK) & 0xFFFFu) == 0xFFFFu;
          request.edram_stencil_clear = ProveStencilClearOwnership(proof);
          ProveFullOverwrite(request, proof);
          if ((proof.depth_control & 7) == 1 && stencil_clear_diagnostics_++ < 16)
            REXLOG_INFO("[native] canonical stencil clear candidate: VS={} PS={} eligible={} reason={} "
                        "aaMask={:08X} physical={}x{} tiles={} ref={}",
                        request.vs->number, request.ps->number, request.edram_stencil_clear.eligible,
                        request.edram_stencil_clear.reason, Register(rex::graphics::XE_GPU_REG_PA_SC_AA_MASK),
                        request.edram_stencil_clear.physical_width, request.edram_stencil_clear.physical_height,
                        request.edram_stencil_clear.length_tiles, request.edram_stencil_clear.stencil_reference);
        }
        if (extent_diagnostics_.size() < 32) {
          const std::string key = fmt::format("{:X}/{:X}/{}/{}/{}", current_vs_hash_,
              Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
              Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO),
              request.edram_used_height_estimate.value_or(UINT32_MAX), diagnostic.reason);
          if (extent_diagnostics_.insert(key).second)
            REXLOG_INFO("[native] bounded draw extent: code={:016X} VS={} depth={:08X} surface={:08X} "
                        "accepted={} maxY={} reason={} format={} postVS=[{},{},{},{}; {},{},{},{}; {},{},{},{}]",
                        current_vs_hash_, request.vs->number,
                        Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                        Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO),
                        bool(request.edram_used_height_estimate),
                        request.edram_used_height_estimate.value_or(UINT32_MAX), diagnostic.reason,
                        diagnostic.rejected_format, diagnostic.position[0][0], diagnostic.position[0][1], diagnostic.position[0][2], diagnostic.position[0][3],
                        diagnostic.position[1][0], diagnostic.position[1][1], diagnostic.position[1][2], diagnostic.position[1][3],
                        diagnostic.position[2][0], diagnostic.position[2][1], diagnostic.position[2][2], diagnostic.position[2][3]);
        }
      }
    }
    ++native_draw_attempts_;
    if (!targets_->Draw(request)) {
      if (++native_draw_failures_ <= 8)
        REXLOG_WARN("[native] draw rejected before completion; visual conformance is not established");
    }
  }

  void Copy() {
    if (!EnsureTargets()) return;
    namespace g = rex::graphics;
    masseffect::native::RegistersCopy r;
    r.rb_surface_info = Register(g::XE_GPU_REG_RB_SURFACE_INFO);
    r.rb_color_info[0] = Register(g::XE_GPU_REG_RB_COLOR_INFO);
    r.rb_color_info[1] = Register(g::XE_GPU_REG_RB_COLOR1_INFO);
    r.rb_color_info[2] = Register(g::XE_GPU_REG_RB_COLOR2_INFO);
    r.rb_color_info[3] = Register(g::XE_GPU_REG_RB_COLOR3_INFO);
    r.rb_depth_info = Register(g::XE_GPU_REG_RB_DEPTH_INFO);
    r.rb_copy_control = Register(g::XE_GPU_REG_RB_COPY_CONTROL);
    r.rb_copy_dest_base = Register(g::XE_GPU_REG_RB_COPY_DEST_BASE);
    r.rb_copy_dest_pitch = Register(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    r.rb_copy_dest_info = Register(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    r.rb_color_clear = Register(g::XE_GPU_REG_RB_COLOR_CLEAR);
    r.rb_color_clear_lo = Register(g::XE_GPU_REG_RB_COLOR_CLEAR_LO);
    r.rb_depth_clear = Register(g::XE_GPU_REG_RB_DEPTH_CLEAR);
    r.pa_sc_window_offset = Register(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    r.pa_sc_window_scissor_tl = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    r.pa_sc_window_scissor_br = Register(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    r.pa_su_sc_mode_cntl = Register(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    r.pa_su_vtx_cntl = Register(g::XE_GPU_REG_PA_SU_VTX_CNTL);
    r.fetch_vertices[0] = Register(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    r.fetch_vertices[1] = Register(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1);
    ++native_copy_attempts_;
    if (!targets_->Copy(r)) {
      if (++native_copy_failures_ <= 8)
        REXLOG_WARN("[native] copy/clear rejected before completion; visual conformance is not established");
    }
  }

  void Present() {
    if (!presenter_ || !EnsureTargets()) return;
    if (gamma_sent_version_ != gamma_version_) {
      gamma_sent_version_ = gamma_version_;
      targets_->RampGamma(gamma_ramp_);
      if (!gamma_logged_) {
        gamma_logged_ = true;
        REXLOG_INFO("[native] game gamma ramp: {} writes, [0]={}/{}/{}, [64]={}/{}/{}, "
                    "[128]={}/{}/{}, [255]={}/{}/{}",
                    gamma_writes_, gamma_ramp_[0][0], gamma_ramp_[0][1], gamma_ramp_[0][2],
                    gamma_ramp_[64][0], gamma_ramp_[64][1], gamma_ramp_[64][2],
                    gamma_ramp_[128][0], gamma_ramp_[128][1], gamma_ramp_[128][2],
                    gamma_ramp_[255][0], gamma_ramp_[255][1], gamma_ramp_[255][2]);
      }
    }
    masseffect::native::TextureSwap texture;
    for (uint32_t i = 0; i < 6; ++i) {
      texture.dword[i] = Register(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i);
    }
    if (targets_->Present(presenter_.get(), texture, swap_width_, swap_height_)) {
      ++presented_;
    }
  }

  void Report(bool force) {
    const auto now = Clock::now();
    if (!force && now - last_report_ < std::chrono::seconds(10)) return;
    const double secs = std::chrono::duration<double>(now - last_report_).count();
    last_report_ = now;
    // GPU time per category, real milliseconds per Swap of this interval: raw NVK timestamps x 1.627 on the Switch.
    if (targets_) {
      std::array<uint64_t, masseffect::native::kGpuCategories> gpu{}, frag{}, vert{}, prim{};
      targets_->TimeGpuPerCategory(gpu);
      targets_->StatsPipeline(frag, vert, prim);
      constexpr double kScale = 1.627;
      const uint64_t swaps = swaps_ - report_gpu_swaps_;
      if (swaps) {
        static constexpr const char* kNames[masseffect::native::kGpuCategories] = {
            "other",  "shadows",    "scene",        "640",         "320",         "smaller",     "copies",
            "clears", "scene_no_z", "gap",         "edram_import", "edram_export", "edram_alias", "edram_import9"};
        std::string t;
        double total = 0;
        for (uint32_t i = 0; i < masseffect::native::kGpuCategories; ++i) {
          const double ms = double(gpu[i] - report_gpu_ns_[i]) * kScale / 1e6 / double(swaps);
          total += ms;
          t += fmt::format(" {} {:.1f} ms ({:.2f} Mfrag)", kNames[i], ms,
                           double(frag[i] - report_gpu_frag_[i]) / 1e6 / double(swaps));
        }
        REXLOG_INFO("[native] GPU per Swap ({} Swaps): total {:.1f} ms;{}", swaps, total, t);
        REXLOG_INFO("[native] overwrite proof: discard {} samplemask {} emptyrect {} depthstencil {} noslot {} ok {}",
                    overwrite_rejects_[0], overwrite_rejects_[1], overwrite_rejects_[2], overwrite_rejects_[3],
                    overwrite_rejects_[4], overwrite_rejects_[5]);
        overwrite_rejects_ = {};
      }
      report_gpu_ns_ = gpu;
      report_gpu_frag_ = frag;
      report_gpu_swaps_ = swaps_;
    }
    {  // PM4 mix of this interval (what the ring parses per frame): type 0 and the busiest type 3 opcodes
      std::array<std::pair<uint64_t, uint32_t>, 128> order;
      for (uint32_t i = 0; i < 128; ++i) order[i] = {opcodes_[i], i};
      std::partial_sort(order.begin(), order.begin() + 8, order.end(), std::greater<>());
      std::string mix;
      for (uint32_t i = 0; i < 8 && order[i].first; ++i)
        mix += fmt::format(" {:02X}:{}pk/{}w", order[i].second, order[i].first, opcode_words_[order[i].second]);
      REXLOG_INFO("[native] PM4 mix (cumulative): {} packets, type 0: {} ({} words, {} single-register); type 3 opcodes "
                  "(packets/words):{}", packets_, type0_packets_, type0_words_, type0_single_, mix);
    }
    REXLOG_INFO("[native] shader loads reused (identical reload): {}; multi-rect draws proven: {} (rejected: "
                "estimator {}, overwrite proof {}, slot mismatch {}); pixel-exact bounds: {}; identity memo hits {}",
                shader_loads_reused_,
                multi_rect_proven_, multi_rect_rejects_[0], multi_rect_rejects_[1], multi_rect_rejects_[2],
                exact_edges_, identity_memo_hits_);
    identity_memo_hits_ = 0;
    multi_rect_proven_ = 0;
    exact_edges_ = 0;
    multi_rect_rejects_ = {};
    shader_loads_reused_ = 0;
    REXLOG_INFO("[native] PS identity audit: {} mismatching candidate checks "
                "(all selection paths, cumulative; not distinct draws)", ps_identity_mismatches_);
    REXLOG_INFO("[native] VS identity audit: {} mismatching candidate checks "
                "(declared fetch patches only; all ALU/CF words compared; cumulative)", vs_identity_mismatches_);
    REXLOG_INFO("[native] FINAL VS selection audit: {} proven, {} unproven, {} missing "
                "(cumulative attempted draws; not GPU/DEF/ABI equivalence proof)",
                vs_final_proven_, vs_final_unproven_, vs_final_missing_);
    uint64_t copies = 0, clears = 0, presented = 0, rejected = 0;
    if (targets_) targets_->Stats(copies, clears, presented, rejected);
    const auto st = targets_ ? targets_->StatsOfDraws() : masseffect::native::StatsDraws{};
    REXLOG_INFO("[native] completion coverage: draw failures {}/{}; copy/clear failures {}/{} "
                "(includes target preparation; successful returns alone do not certify rendering)",
                native_draw_failures_, native_draw_attempts_, native_copy_failures_, native_copy_attempts_);
    REXLOG_INFO("[native] {:.1f} s: {} Swaps, {} draws ({} without shaders), {} copies, {} packets, "
                "{} interrupts, {} WAIT_REG_MEM timeouts; shaders {} identified ({} by address) / {} not; native: "
                "{} drawn, {} rejected, {} pipelines, {} textures, {} presented",
                secs, swaps_ - reported_swaps_, draws_ - reported_draws_, draws_unidentified_,
                copies_ - reported_copies_, packets_ - reported_packets_,
                interrupts_.load() - reported_interrupts_, waits_timed_out_, shaders_identified_,
                shaders_by_address_, shaders_unidentified_, st.drawn, st.rejected, st.pipelines, st.textures,
                presented);
    HangWatchdogInterval(swaps_ - reported_swaps_, draws_ - reported_draws_);
    std::string causes;
    for (size_t i = 0; i < st.causes.size() && i < 6; ++i) {
      causes += fmt::format(" {}x{}", st.causes[i].first, st.causes[i].second);
    }
    if (!causes.empty()) REXLOG_INFO("[native] rejection causes (code x count):{}", causes);
    if (!missing_draw_pairs_.empty()) {
      std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint64_t>> ranked(
          missing_draw_pairs_.begin(), missing_draw_pairs_.end());
      std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
      });
      std::string missing;
      for (size_t i = 0; i < ranked.size() && i < 12; ++i) {
        missing += fmt::format(" {}x(VS={:016X},PS={:016X})", ranked[i].second,
                               ranked[i].first.first, ranked[i].first.second);
      }
      REXLOG_INFO("[native] unresolved draw shader pairs (cumulative, 0 means identified):{}", missing);
    }
    {
      std::lock_guard<std::mutex> lock(g_by_address_mutex);
      REXLOG_INFO("[native] tile replays reusing a pairing {}, draws dropped by predicate {}",
                  draws_replayed_, draws_predicated_);
      std::string miss;
      for (auto& [o, n] : missing_objects_) miss += fmt::format(" {:08X}x{}", o, n);
      REXLOG_INFO("[native] draws paired by command-buffer address {} (no VS entry {}, no PS entry {});"
                  " objects without an entry:{}", draws_paired_by_address_, paired_no_vs_, paired_no_ps_, miss);
      REXLOG_INFO("[native] draws paired with a Draw* record {}, unpaired {}; "
                  "draws inside indirect buffers {} of {}; Draw* records {}",
                  draws_paired_, draws_unpaired_, draws_indirect_, draws_,
                  g_draw_records.load());
      REXLOG_INFO("[native] identified by code {}; unidentified loads: VS by address {}, PS by address "
                  "{}, immediate {}; {} objects seen, {} addresses and {} codes mapped, {} code conflicts",
                  shaders_by_code_, miss_vs_addr_, miss_ps_addr_, miss_immediate_, g_objects_seen.size(),
                  g_by_address.size(), g_by_code.size(), g_code_conflicts);
    }
    reported_swaps_ = swaps_;
    reported_draws_ = draws_;
    reported_copies_ = copies_;
    reported_packets_ = packets_;
    reported_interrupts_ = interrupts_.load();
  }

  masseffect::native::ShadersNative shaders_;
  std::unique_ptr<masseffect::native::TargetsNative> targets_;
  bool targets_failed_ = false;
  const masseffect::native::ShaderEntry* vs_ = nullptr;
  const masseffect::native::ShaderEntry* ps_ = nullptr;
  std::vector<uint32_t> microcode_, vs_microcode_, ps_microcode_;
  uint64_t gen_vs_ = 0, gen_microcode_ = 0, gen_constants_vs_ = 0, gen_constants_ps_ = 0,
           gen_fetch_ = 0, gen_viewport_ = 0;
  uint32_t swap_width_ = 1280, swap_height_ = 720;
  uint32_t logged_misses_ = 0;
  uint32_t im_load_address_ = 0;
  uint64_t shaders_by_code_ = 0, draws_paired_ = 0, draws_unpaired_ = 0;
  uint64_t native_draw_attempts_ = 0, native_draw_failures_ = 0;
  std::unique_ptr<NativeDrawExtentEstimator> draw_extent_estimator_;
  std::unordered_set<std::string> extent_diagnostics_;
  uint32_t stencil_clear_diagnostics_ = 0;
  uint64_t native_copy_attempts_ = 0, native_copy_failures_ = 0;
  uint32_t logged_unpaired_ = 0;
  uint64_t draws_paired_by_address_ = 0, paired_no_vs_ = 0, paired_no_ps_ = 0;
  std::unordered_map<uint32_t, uint64_t> missing_objects_;
  std::unordered_set<uint64_t> dumped_rewritten_;
  std::array<uint64_t, masseffect::native::kGpuCategories> report_gpu_ns_{}, report_gpu_frag_{};
  uint64_t report_gpu_swaps_ = 0;
  std::unordered_set<uint64_t> dumped_vertex_variants_;
  uint64_t draws_indirect_ = 0, draws_replayed_ = 0, draws_predicated_ = 0;
  uint32_t packet_address_ = 0;
  struct PairedPacket {
    uint64_t frame;
    const masseffect::native::ShaderEntry* vs;
    const masseffect::native::ShaderEntry* ps;
  };
  std::unordered_map<uint32_t, PairedPacket> paired_packets_;
  std::deque<DrawRecord> pending_;
  const masseffect::native::ShaderEntry* draw_vs_ = nullptr;
  const masseffect::native::ShaderEntry* draw_ps_ = nullptr;
  uint64_t shaders_by_address_ = 0, miss_vs_addr_ = 0, miss_ps_addr_ = 0, miss_immediate_ = 0;
  uint64_t draws_unidentified_ = 0, shaders_identified_ = 0, shaders_unidentified_ = 0, presented_ = 0;
  uint64_t current_vs_hash_ = 0, current_ps_hash_ = 0;
  uint32_t last_vs_load_address_ = UINT32_MAX, last_ps_load_address_ = UINT32_MAX;
  uint64_t shader_loads_reused_ = 0;
  uint64_t ps_identity_mismatches_ = 0;
  uint64_t vs_identity_mismatches_ = 0;
  uint64_t vs_final_proven_ = 0, vs_final_unproven_ = 0, vs_final_missing_ = 0;
  uint64_t vs_identity_generation_ = 1;
  uint64_t ps_identity_generation_ = 1;
  std::unordered_map<const masseffect::native::ShaderEntry*, std::pair<uint64_t, bool>> vs_identity_cache_;
  std::array<IdentityCell, kIdentityCells> vs_cells_{}, ps_cells_{}, fc_cells_{};
  // Ring CPU switches (masseffect_native_flat_identity, _fast_pair, _raw_microcode, _pm4_fast) and the
  // remaining self-check uses of each.
  bool identity_flat_ = false, pairing_fast_ = false, raw_microcode_ = false, pm4_fast_ = false;
  uint32_t check_identity_ = 0, check_pairing_ = 0, check_raw_ = 0, check_pm4_ = 0, check_objects_ = 0;
  std::vector<uint32_t> raw_vs_, raw_ps_;  // the guest words of the last IM_LOAD of each stage (raw_microcode_)
  bool raw_vs_valid_ = false, raw_ps_valid_ = false;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> missing_draw_pairs_;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  bool mmio_registered_ = false;
  std::atomic<bool> active_{false};
  std::vector<uint32_t> registers_;
  std::array<std::array<uint16_t, 3>, 256> gamma_ramp_ = [] {
    std::array<std::array<uint16_t, 3>, 256> ramp{};
    for (uint32_t i = 0; i < 256; ++i) ramp[i].fill(uint16_t(i * 0x3FF / 0xFF));
    return ramp;
  }();
  uint32_t gamma_component_ = 0;
  uint32_t gamma_mask_ = 0b111;
  uint64_t gamma_version_ = 0, gamma_sent_version_ = 0, gamma_writes_ = 0;
  bool gamma_logged_ = false;
  std::atomic<uint32_t> callback_{0}, callback_data_{0};
  std::atomic<uint32_t> ring_base_{0}, ring_words_{0}, ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0}, read_writeback_{0};
  std::atomic<uint32_t> counter_{0};
  std::mutex ring_mutex_;
  std::atomic<bool> ring_waiting_{false};
  std::condition_variable ring_cv_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_, vblank_thread_;
  uint64_t bin_mask_ = ~0ull, bin_select_ = ~0ull;
  // Ring-thread counters.
  uint64_t packets_ = 0, draws_ = 0, copies_ = 0, swaps_ = 0, waits_timed_out_ = 0;
  uint64_t opcodes_[128] = {};
  uint64_t opcode_words_[128] = {}, type0_packets_ = 0, type0_words_ = 0, type0_single_ = 0;  // PM4 mix report
  std::atomic<uint64_t> interrupts_{0};
  uint64_t reported_swaps_ = 0, reported_draws_ = 0, reported_copies_ = 0, reported_packets_ = 0,
           reported_interrupts_ = 0;
  Clock::time_point start_, last_report_;
};

}  // namespace

bool HasEntry(uint32_t object) {
  std::lock_guard<std::mutex> lock(g_by_address_mutex);
  return g_entry_of_object.count(object) != 0;
}

void NoteShaderObject(const uint8_t* base, uint32_t object, bool vertex) {
  const masseffect::native::ShadersNative* library = g_library.load(std::memory_order_acquire);
  if (!library || object < 0x40000000 || object >= 0x7F000000) return;
  auto load32 = [base](uint32_t a) { return rex::memory::load_and_swap<uint32_t>(base + a); };
  if ((load32(object) & 0xFF) != (vertex ? 6u : 7u)) return;
  // Objects whose work is finished (seen and code-mapped: the locked path below only returns for them) are
  // remembered per game thread, so the per-draw call skips the shared mutex (contended with the ring thread).
  thread_local std::array<uint32_t, 512> finished_cache{};
  uint32_t& finished = finished_cache[(object >> 4) & 511];
  if (finished == object) return;
  {
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    if (!g_objects_seen.insert(object).second) {
      // Already identified: map its current microcode once (for VS, after D3D patched it).
      auto it = g_entry_of_object.find(object);
      if (it != g_entry_of_object.end() && g_code_mapped.count(object)) finished = object;
      // Seen but never identified: only the first sighting identifies an object (below), so this stays
      // unidentified; remember it too instead of taking the mutex on every draw (~0.5 % of the render thread).
      if (it == g_entry_of_object.end()) finished = object;
      if (it == g_entry_of_object.end() || g_code_mapped.count(object)) return;
      const uint32_t mc = load32(object + (vertex ? 0x20 : 0x18));
      const uint32_t header = object + (vertex ? 0x368 : 0x28);
      const uint32_t sh = header + load32(header + 24);
      const uint32_t mc_offset = load32(sh), mc_bytes = load32(sh + 4);
      if (mc < 0xA0000000u || !mc_bytes || mc_bytes > 0x40000) return;
      const uint64_t h = CodeHash(base + mc + mc_offset, mc_bytes / 4);
      auto [slot, inserted] = g_by_code.emplace(h, it->second);
      if (!inserted && slot->second != it->second) ++g_code_conflicts;
      g_code_mapped.insert(object);
      finished = object;
      return;
    }
  }
  // Same layout as src/me_shader_dump.cpp: container copy in the object, microcode pointer.
  const uint32_t header = object + (vertex ? 0x368 : 0x28);
  const uint32_t virtual_size = load32(header + 4);
  const uint32_t physical_size = load32(header + 8);
  const uint32_t microcode = load32(object + (vertex ? 0x20 : 0x18));
  if ((load32(header) & 0xFFFFFF00u) != 0x102A1100u || virtual_size < 28 || virtual_size > 0x40000 ||
      !physical_size || physical_size > 0x40000 || microcode < 0xA0000000u) {
    return;
  }
  std::vector<uint8_t> container(virtual_size + physical_size);
  std::memcpy(container.data(), base + header, virtual_size);
  std::memcpy(container.data() + virtual_size, base + microcode, physical_size);
  const masseffect::native::ShaderEntry* entry = library->IdentifyContainer(container);
  if (!entry && vertex) {
    std::lock_guard<std::mutex> lock(g_by_address_mutex);
    if (!g_vs_by_virtual_built) BuildVirtualIndex(library);
    auto it = g_vs_by_virtual.find(XXH3_64bits(container.data(), virtual_size));
    if (it != g_vs_by_virtual.end()) entry = it->second;
  }
  // Direct3D's internal containers may not be in the package byte-for-byte, because discovery
  // replaces their incomplete virtual metadata with a conservative register-linked wrapper.
  // Their physical microcode is still authoritative. The library code index deliberately prefers
  // the complete wrapper when the same code occurs in multiple containers.
  if (!entry) {
    const uint32_t shader_header = virtual_size >= 28 ? load32(header + 24) : 0;
    const uint32_t physical_offset =
        shader_header + 8 <= virtual_size ? load32(header + shader_header) : 0;
    const uint32_t shader_bytes =
        shader_header + 8 <= virtual_size ? load32(header + shader_header + 4) : 0;
    if (shader_bytes && !(shader_bytes & 3) &&
        uint64_t(physical_offset) + shader_bytes <= physical_size) {
      const uint64_t code = CodeHash(container.data() + virtual_size + physical_offset,
                                     shader_bytes / 4) ^ (vertex ? 1ull : 0ull);
      if (auto it = g_library_by_code.find(code); it != g_library_by_code.end()) {
        entry = it->second;
        REXLOG_INFO("[native] shader object {:08X} matched register-linked {} n{} by microcode",
                    object, vertex ? "VS" : "PS", entry->number);
      }
    }
  }
  if (!entry) {
    const uint32_t shader_header = virtual_size >= 28 ? load32(header + 24) : 0;
    const uint32_t physical_offset = shader_header + 8 <= virtual_size ? load32(header + shader_header) : 0;
    const uint32_t shader_bytes = shader_header + 8 <= virtual_size ? load32(header + shader_header + 4) : 0;
    REXLOG_INFO("[native] shader object {:08X} not in the library: {}_{:016x}; mc {:08X}, "
                "virtual {} physical {}, shader header +{:X}, code +{:X}/{} -> ring {:08X}",
                object, vertex ? "vs" : "ps", XXH3_64bits(container.data(), container.size()),
                microcode, virtual_size, physical_size, shader_header, physical_offset, shader_bytes,
                (microcode & 0x1FFFFFFFu) + physical_offset);
    // Not in the library (containers Direct3D or the engine build at startup): keep a copy with
    // MASSEFFECT_SHADER_MISSING=<folder> so the library can be extended (shaders/tools/build_shader_spirv.sh).
    if (const char* folder = std::getenv("MASSEFFECT_SHADER_MISSING"); folder && *folder) {
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      const uint64_t h = XXH3_64bits(container.data(), container.size());
      const auto path = std::filesystem::path(folder) /
                        fmt::format("{}_{:016x}.bin", vertex ? "vs" : "ps", h);
      if (std::FILE* f = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(container.data(), 1, container.size(), f);
        std::fclose(f);
      }
    }
    return;
  }
  const uint32_t shader_header = rex::memory::load_and_swap<uint32_t>(container.data() + 24);
  const uint32_t physical_offset =
      shader_header + 4 <= virtual_size
          ? rex::memory::load_and_swap<uint32_t>(container.data() + shader_header)
          : 0;
  const uint32_t mc_bytes = shader_header + 8 <= virtual_size
                                ? rex::memory::load_and_swap<uint32_t>(container.data() + shader_header + 4)
                                : 0;
  std::lock_guard<std::mutex> lock(g_by_address_mutex);
  g_by_address[(microcode & 0x1FFFFFFFu) + physical_offset] = entry;
  g_entry_of_object[object] = entry;
  PublishObject(object, entry);
  if (!vertex) {  // pixel shaders are never patched: map their microcode now
    if (mc_bytes && virtual_size + physical_offset + mc_bytes <= container.size()) {
      const uint64_t h = CodeHash(container.data() + virtual_size + physical_offset, mc_bytes / 4);
      auto [slot, inserted] = g_by_code.emplace(h, entry);
      if (!inserted && slot->second != entry) ++g_code_conflicts;
      g_code_mapped.insert(object);
    }
  }
}

void NoteDrawCall(const uint8_t* base, uint32_t type, uint32_t r5, uint32_t r6, uint32_t r7) {
  if (!g_native_active.load(std::memory_order_acquire)) return;
  constexpr uint32_t kDevice = 0x4004B000;  // the D3D device object
  const uint32_t vs = rex::memory::load_and_swap<uint32_t>(base + kDevice + 12416);
  const uint32_t ps = rex::memory::load_and_swap<uint32_t>(base + kDevice + 12412);
  NoteShaderObject(base, vs, true);
  NoteShaderObject(base, ps, false);
  g_draw_records.fetch_add(1, std::memory_order_relaxed);
  const uint32_t start = rex::memory::load_and_swap<uint32_t>(base + kDevice + 48) & 0x1FFFFFFFu;
  DrawRecord record{type, r5, r6, r7, vs, ps, start};
  if (RecordTable()) {
    PushRecord(std::move(record), start);  // lock-free; the ring inserts it into its table
    return;
  }
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  g_draw_by_address[start] = std::move(record);
  if (g_draw_by_address.size() > 65536) g_draw_by_address.erase(g_draw_by_address.begin());
}

bool Enabled() {
  // Horizon does not launch an NRO with a useful process environment: the toml decides. Native by default;
  // masseffect_renderer_native = false + gpu_plugin = "xenos" runs ReXGlue's Xenos emulation instead.
  return REXCVAR_GET(masseffect_renderer_native);
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace me::native
