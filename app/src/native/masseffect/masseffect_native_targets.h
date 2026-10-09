// masseffect - native renderer: render targets, copies (resolve and clear) and presentation of the game's
// image.
//
// Each Swap presents the destination of the last copy (RB_COPY_DEST_BASE), and during the videos and the
// title screen there are 2 copies per frame, both with a clear. This part does that without EDRAM: a render
// target is one Vulkan image per (base, format, pitch), and a resolved texture is another image per
// destination address, which is where the Swap looks it up through its fetch constant.

#pragma once

#include "masseffect_native_draws.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanDevice;
}

namespace masseffect::native {

// Registers a copy needs, taken from the ring sink's register mirror.
struct RegistersCopy {
  uint32_t rb_surface_info = 0;
  uint32_t rb_color_info[4] = {};
  uint32_t rb_depth_info = 0;
  uint32_t rb_copy_control = 0;
  uint32_t rb_copy_dest_base = 0;
  uint32_t rb_copy_dest_pitch = 0;
  uint32_t rb_copy_dest_info = 0;
  uint32_t rb_color_clear = 0;
  uint32_t rb_color_clear_lo = 0;
  uint32_t rb_depth_clear = 0;
  uint32_t pa_sc_window_offset = 0;
  uint32_t pa_sc_window_scissor_tl = 0;
  uint32_t pa_sc_window_scissor_br = 0;
  uint32_t pa_su_sc_mode_cntl = 0;
  uint32_t pa_su_vtx_cntl = 0;
  // Fetch constant 0 as vertices: the copy rectangle (D3D9 puts it there).
  uint32_t fetch_vertices[2] = {};
};

// Fetch constant 0 as a texture at the time of the Swap: VdSwap writes it.
struct TextureSwap {
  uint32_t dword[6] = {};
};

// D3D9 occlusion query result in guest memory (the end structure that RB_SAMPLE_COUNT_ADDR points to, 8 words,
// little endian: D3D reads them with lwbrx). D3D's GetData (sub_82229158 in the English edition) treats the
// query as not finished while words 0..3 (Total_A/B, ZFail_A/B) all hold the byte-swapped 0xFFFFFEED sentinel,
// and returns (end.ZPass_A + end.ZPass_B) - (begin.ZPass_A + begin.ZPass_B), begin being the structure at +0x20.
// The ZPass words are written first and the sentinel words last, so a reader that sees the query finished
// never reads a half-written count.
inline void WriteOcclusionCounts(uint8_t* guest, uint32_t samples) {
  if (!guest) return;
  const auto put = [guest](uint32_t offset, uint32_t value) {
    const uint8_t bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    std::memcpy(guest + offset, bytes, 4);
  };
  put(16, samples);  // ZPass_A
  put(20, 0);        // ZPass_B
  put(24, 0);        // StencilFail_A
  put(28, 0);        // StencilFail_B
  std::atomic_thread_fence(std::memory_order_release);
  put(4, 0);         // Total_B
  put(8, 0);         // ZFail_A
  put(12, 0);        // ZFail_B
  put(0, samples);   // Total_A
  std::atomic_thread_fence(std::memory_order_release);
}

// Real occlusion queries (masseffect_native_query_mode 2 and 3), accumulated since the start.
struct StatsQueries {
  uint64_t ended = 0;          // guest queries handed to QueryEnd
  uint64_t resolved = 0;       // written with the real GPU count
  uint64_t resolved_zero = 0;  // of those, 0 samples (the game will skip the primitive)
  uint64_t fallback_unmeasured = 0;  // a draw inside the query was not measured (dropped, rejected, no room)
  uint64_t fallback_split = 0;       // the query spanned a submission
  uint64_t fallback_timeout = 0;     // no GPU result within masseffect_native_query_timeout_us
  uint64_t fallback_read = 0;        // vkGetQueryPoolResults failed
  uint64_t superseded = 0;           // the guest reissued the query before its result was known
  uint64_t empty = 0;                // no draw inside the query: 0 samples, as on the console
  uint64_t vulkan_queries = 0;       // per-draw Vulkan occlusion queries recorded
  uint64_t flushes = 0;              // submissions made early so a waiting query can finish
  uint64_t latency_frames = 0;       // sum over resolved queries of Swaps between the end and the result
  uint64_t latency_us = 0;           // the same in microseconds
  uint64_t samples = 0;              // sum of the counts written (after scaling)
  // Latency-1 queries (masseffect_native_query_mode 3): answers given at the end packet from the history table,
  // and the measurements that feed the table (resolved/resolved_zero/samples/latency above count the folds).
  uint64_t answered_history = 0;   // answered from a fresh table entry (hidden or its last non-zero count)
  uint64_t answered_hidden = 0;    // of those, answered 0 (enough consecutive zero results)
  uint64_t answered_unknown = 0;   // no identity or no entry: answered "visible"
  uint64_t answered_stale = 0;     // entry older than masseffect_native_query_max_age frames: "visible"
  uint64_t latent_measured = 0;    // issues measured with Vulkan queries for the table
  uint64_t latent_unmeasured = 0;  // issues not measured (split, a draw not measured, no identity)
  uint64_t content_moved = 0;      // the same box content seen at another end structure (pooled query objects)
  uint64_t history_size = 0;       // table entries now (not accumulated)
};

class TargetsNative {
 public:
  // nullptr if the device is missing or presentation could not be set up.
  static std::unique_ptr<TargetsNative> Create(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                                                rex::memory::Memory* memory);
  virtual ~TargetsNative() = default;

  // Resolve and/or clear. false if it could not be done (each cause is logged once).
  virtual bool Copy(const RegistersCopy& registers) = 0;

  // Submits pending work and draws the Swap's texture into the presenter's output. false if that Swap has
  // no resolved texture.
  virtual bool Present(rex::ui::Presenter* presenter, const TextureSwap& swap, uint32_t width,
                         uint32_t height) = 0;

  // 256-entry gamma ramp loaded by the game, 10 bits per channel (red, green, blue). Subsequent outputs
  // apply it the way the Xbox 360 display does. Called from the same thread as Present.
  virtual void RampGamma(const std::array<std::array<uint16_t, 3>, 256>& ramp) = 0;

  // Records a ring draw with its shaders. false if it could not.
  virtual bool Draw(const SubmissionDraw& submission) = 0;
  virtual StatsDraws StatsOfDraws() const = 0;
  // Deferred vertex copies finished: before returning the read pointer to the game.
  virtual void WaitUploads() = 0;
  // Vertex copies queued and not done yet (only to measure the fences).
  virtual size_t PendingCopies() const { return 0; }
  // docs/memory-growth.md: appends " name count (N KB)" items for the long-lived host caches (render targets and
  // the draws' caches). Ring thread, every masseffect_mem_report_s.
  virtual void MemoryCaches(std::string& out) { (void)out; }
  // Copies, clears, presented Swaps and rejected operations, accumulated.
  virtual void Stats(uint64_t& copies, uint64_t& cleared, uint64_t& presented,
                            uint64_t& rejections) const = 0;
  // GPU nanoseconds accumulated per category (kGpuOther..., masseffect_native_draws.h).
  virtual void TimeGpuPerCategory(
      std::array<uint64_t, kGpuCategories>& nanoseconds) const = 0;
  // Shaded fragments, vertex invocations and clipped primitives, accumulated per pass category (only
  // with masseffect_native_stats_pipeline).
  virtual void StatsPipeline(std::array<uint64_t, kGpuCategories>& fragments,
                                    std::array<uint64_t, kGpuCategories>& vertices,
                                    std::array<uint64_t, kGpuCategories>& primitives) const = 0;

  // Real occlusion queries (masseffect_native_query_mode 2 and 3). All on the ring thread.
  // A guest query begins (EVENT_WRITE_ZPD on the begin structure): every draw recorded until QueryEnd is
  // wrapped in its own Vulkan occlusion query (SubmissionDraw::occlusion_query).
  virtual void QueryBegin() {}
  // The guest query ends with `draws` draws issued inside it. true: the end structure at `address` keeps the
  // D3D sentinel and gets the real count (x scale) once the GPU has it, or "visible" on failure or timeout.
  // false: nothing is pending, the caller writes the fallback now.
  virtual bool QueryEnd(uint32_t address, uint32_t draws, double scale, uint32_t visible) {
    (void)address, (void)draws, (void)scale, (void)visible;
    return false;
  }
  // The guest issues the query at this end structure again: a result still pending for it is dropped.
  virtual void QueryForget(uint32_t address) { (void)address; }
  // Writes the results of finished submissions; when `idle` (the ring has parsed everything written), submits
  // the recorded work early if a finished guest query waits in it for at least flush_us; results older than
  // timeout_us become "visible".
  virtual void QueryService(bool idle, uint32_t flush_us, uint32_t timeout_us) {
    (void)idle, (void)flush_us, (void)timeout_us;
  }
  // Latency-1 queries (masseffect_native_query_mode 3): the guest query at `address` ends; the return value is
  // written into its end structure at once (the game never waits). The answer comes from the most recent folded
  // GPU result of the same identity `key` (0 = no identity: "visible"): `visible` if unknown or older than
  // max_age_frames Swaps, 0 only after hidden_after consecutive zero results, else the last non-zero count.
  // The draws of this issue are measured as in mode 2 when possible; ReadOcclusion folds the result into the
  // table when the slot's fence has signaled (no early submission, no timeout). `content` (0 = none) is the box
  // content alone, only for the pooling diagnostic.
  virtual uint32_t QueryEndLatent(uint32_t address, uint64_t key, uint64_t content, uint32_t draws, double scale,
                                  uint32_t visible, uint32_t max_age_frames, uint32_t hidden_after) {
    (void)address, (void)key, (void)content, (void)draws, (void)scale, (void)max_age_frames, (void)hidden_after;
    return draws ? visible : 0u;
  }
  // Mode 2: guest results waiting for the GPU. Mode 3: measurements not yet folded into the table.
  virtual bool QueriesPending() const { return false; }
  virtual StatsQueries StatsOfQueries() const { return {}; }
};

}  // namespace masseffect::native
