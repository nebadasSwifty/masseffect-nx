// Deferred Vulkan command recording: see masseffect_deferred_recording.h.
#include "masseffect_deferred_recording.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <cstring>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>

#include <switch.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(masseffect_native_deferred_recording, false, "Mass Effect",
                    "Ring thread queues every vkCmd* call and a worker thread records them into the same command "
                    "buffers in the same order (moves NVK recording off the ring thread; same commands, same image)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_deferred_recording_parts, 3, "Mass Effect",
                     "Diagnostic bisection of masseffect_native_deferred_recording: 1 = proxy function table, "
                     "2 = wrappers for the functions fetched by name")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_deferred_recording_naps, 64, "Mass Effect",
                     "Worker: 5 us naps on an empty queue before it blocks on the condition variable "
                     "(each nap preempts a game thread on its core)")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(masseffect_deferred_native_fast, false, "Mass Effect",
                    "Queue: the producer publishes commands in batches (masseffect_deferred_native_batch) instead "
                    "of one seq_cst store + wake check per command, keeps its own copy of the worker's read index "
                    "and the worker stores it every 16 commands (less cache line ping-pong between the cores). "
                    "Same commands, same order; false = as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_deferred_native_batch, 16, "Mass Effect",
                     "masseffect_deferred_native_fast: commands queued before the worker is told (the queue is also "
                     "published before the ring thread waits, drains or idles)")
    .range(1, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_deferred_native_update, false, "Mass Effect",
                    "vkUpdateDescriptorSets is queued too (its write arrays deep-copied into the arena) instead of "
                    "draining the worker (~70 drains per frame, each a spin-yield of the ring thread). The update runs "
                    "on the worker at the same position of the command stream; every submit, destroy and reset still "
                    "drains first")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace masseffect::native::deferred {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Queue: single producer (the ring thread), single consumer (the worker).
constexpr size_t kSlots = size_t(1) << 10;  // 2 MB: the process runs within a few MB of its limit
constexpr size_t kLoad = 112;
struct alignas(16) Slot {
  void (*run)(void*) = nullptr;
  alignas(16) unsigned char load[kLoad];
};
Slot* g_queue = nullptr;
alignas(64) std::atomic<uint64_t> g_written{0};
alignas(64) std::atomic<uint64_t> g_read{0};
uint64_t g_written_producer = 0;
// masseffect_deferred_native_fast: what the worker was last told (g_written), the producer's copy of the
// worker's read index (it only reloads the shared one when the queue looks full) and the batch size.
uint64_t g_published = 0;
uint64_t g_read_view = 0;
bool g_fast = false;
uint32_t g_batch = 1;
uint64_t g_batches = 0, g_updates = 0, g_direct_updates = 0;
std::atomic<bool> g_sleeping{false};
std::mutex g_mutex;
std::condition_variable g_cv;

std::atomic<bool> g_active{false};
std::atomic<bool> g_pinned_producer{false};
thread_local bool t_producer = false;  // not std::thread::id: SDK threads are not pthreads

// Arrays of the queued calls (barriers, regions, viewports...). Reset whenever the queue is empty.
constexpr size_t kArena = size_t(512) << 10;
uint8_t* g_arena = nullptr;
size_t g_arena_usage = 0;

// Report.
uint64_t g_queued = 0, g_direct = 0, g_drains = 0, g_drains_with_wait = 0, g_ns_wait = 0;
auto g_report = std::chrono::steady_clock::now();

inline bool IsProducer() {
  return t_producer && g_active.load(std::memory_order_relaxed);
}

void Wake() {
  if (g_sleeping.load(std::memory_order_seq_cst)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cv.notify_one();
  }
}

// Tells the worker about everything queued so far (the batched mode may hold a few commands back).
void PublishAlready() {
  g_published = g_written_producer;
  ++g_batches;
  g_written.store(g_written_producer, std::memory_order_seq_cst);
  Wake();
}

Slot& Reserve() {
  if (g_fast) {
    if (g_written_producer - g_read_view >= kSlots) {
      if (g_published != g_written_producer) PublishAlready();  // the worker cannot free slots it was not told about
      for (;;) {
        g_read_view = g_read.load(std::memory_order_acquire);
        if (g_written_producer - g_read_view < kSlots) break;
        Wake();
        std::this_thread::sleep_for(std::chrono::microseconds(20));
      }
    }
    return g_queue[g_written_producer & (kSlots - 1)];
  }
  while (g_written_producer - g_read.load(std::memory_order_acquire) >= kSlots) {
    Wake();
    std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
  return g_queue[g_written_producer & (kSlots - 1)];
}

void Publish() {
  ++g_written_producer;
  ++g_queued;
  if (g_fast) {
    if (g_written_producer - g_published >= g_batch) PublishAlready();
    return;
  }
  g_written.store(g_written_producer, std::memory_order_seq_cst);
  Wake();
}

int32_t g_priority_producer = -1;

void Worker() {
  uint64_t read = 0;
  uint32_t empty = 0;
  const bool fast = g_fast;
  for (;;) {
    const uint64_t written = g_written.load(std::memory_order_acquire);
    if (read < written) {
      if (fast) {
        // The producer only needs the read index when the queue is full or to drain: every 16 commands and at the
        // end of the burst (drain waits for g_read == g_written_producer, which the final store provides).
        do {
          Slot& r = g_queue[read & (kSlots - 1)];
          r.run(r.load);
          ++read;
          if ((read & 15) == 0) g_read.store(read, std::memory_order_release);
        } while (read < written);
        g_read.store(read, std::memory_order_release);
      } else {
        do {
          Slot& r = g_queue[read & (kSlots - 1)];
          r.run(r.load);
          ++read;
          g_read.store(read, std::memory_order_release);
        } while (read < written);
      }
      empty = 0;
      continue;
    }
    // A short spin (the ring publishes in bursts), then sleep: the three cores are shared with the game.
    static const uint32_t naps = uint32_t(REXCVAR_GET(masseffect_native_deferred_recording_naps));
    if (++empty < naps) {
      std::this_thread::sleep_for(std::chrono::microseconds(5));
      continue;
    }
    std::unique_lock<std::mutex> lock(g_mutex);
    g_sleeping.store(true, std::memory_order_seq_cst);
    if (g_written.load(std::memory_order_seq_cst) == read) {
      g_cv.wait_for(lock, std::chrono::milliseconds(2));
    }
    g_sleeping.store(false, std::memory_order_seq_cst);
    empty = 0;
  }
}

void DrainInternal() {
  static uint32_t traces = 0;
  if (traces < 6) {
    ++traces;
    REXLOG_INFO("[native] deferred recording: drain {}: written {} read {}", traces, g_written_producer,
                g_read.load(std::memory_order_acquire));
  }
  if (g_fast && g_published != g_written_producer) PublishAlready();
  if (g_read.load(std::memory_order_acquire) != g_written_producer) {
    const auto t0 = std::chrono::steady_clock::now();
    Wake();
    // Not yield(): on Horizon it only gives way to threads of the SAME priority on the SAME core, so a worker
    // on this core with a lower priority would never run (the first builds of this worker hung exactly like that).
    for (uint32_t laps = 0; g_read.load(std::memory_order_acquire) != g_written_producer; ++laps) {
      if (laps < 32) std::this_thread::yield();
      else std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    g_ns_wait += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ++g_drains_with_wait;
  }
  ++g_drains;
  g_read_view = g_written_producer;  // the queue is empty (the worker has executed everything)
  g_arena_usage = 0;
  const auto now = std::chrono::steady_clock::now();
  if (now - g_report >= std::chrono::seconds(10)) {
    g_report = now;
    REXLOG_INFO("[native] deferred recording (10 s): {} calls queued, {} direct on the ring thread; {} "
                "drains ({} waiting for the worker, {:.1f} ms in total); {} publishes; vkUpdateDescriptorSets "
                "{} queued, {} direct",
                g_queued, g_direct, g_drains, g_drains_with_wait, double(g_ns_wait) / 1e6, g_batches,
                g_updates, g_direct_updates);
    g_queued = g_direct = g_drains = g_drains_with_wait = g_ns_wait = 0;
    g_batches = g_updates = g_direct_updates = 0;
  }
}

// One arena block for all arrays of one call (a drain inside would reuse memory already copied for it).
uint8_t* Arena(size_t bytes) {
  bytes = (bytes + 15) & ~size_t(15);
  if (bytes > kArena) return nullptr;
  if (g_arena_usage + bytes > kArena) DrainInternal();
  uint8_t* p = g_arena + g_arena_usage;
  g_arena_usage += bytes;
  return p;
}
template <typename T>
size_t Size(uint32_t n) {
  return (sizeof(T) * n + 15) & ~size_t(15);
}
template <typename T>
const T* Copy(uint8_t*& cursor, const T* src_data, uint32_t n) {
  if (!src_data || !n) return src_data;
  T* target = reinterpret_cast<T*>(cursor);
  std::memcpy(target, src_data, sizeof(T) * n);
  cursor += Size<T>(n);
  return target;
}
template <typename T>
bool WithPNext(const T* p, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if (p[i].pNext) return true;
  return false;
}

bool g_no_enqueue = false;  // bisection bit 32: producer and worker set up, calls made at once

template <typename F, typename... A>
void Enqueue(F f, A... a) {
  if (g_no_enqueue) {
    f(a...);
    return;
  }
  struct Package {
    F f;
    std::tuple<A...> args;
  };
  static_assert(sizeof(Package) <= kLoad && alignof(Package) <= 16, "call too large for the queue");
  Slot& r = Reserve();
  new (r.load) Package{f, std::tuple<A...>(a...)};
  r.run = [](void* c) {
    Package* p = static_cast<Package*>(c);
    std::apply(p->f, p->args);
    p->~Package();
  };
  Publish();
}

// ---------------------------------------------------------------------------------------------------------------
// Generic wrappers, one per function of the table (and per name fetched with Proc).
template <typename T>
constexpr bool kPointerData = std::is_pointer_v<T> && std::is_const_v<std::remove_pointer_t<T>>;

enum Mode : uint8_t { kDirect = 0, kDrain = 1, kDefer = 2 };

template <size_t Id, typename PFN>
struct Wrapper;
template <size_t Id, typename R, typename... A>
struct Wrapper<Id, R (*)(A...)> {
  using Pointer = R (*)(A...);
  static constexpr bool kDeferrable = std::is_void_v<R> && !(kPointerData<A> || ...);
  static inline Pointer real = nullptr;
  static inline uint8_t mode = kDirect;
  static R F(A... a) {
    if (IsProducer()) {
      if constexpr (kDeferrable) {
        if (mode == kDefer) {
          Enqueue(real, a...);
          return;
        }
      }
      if (mode != kDirect) {
        DrainInternal();
        ++g_direct;
      }
    }
    return real(a...);
  }
};

uint8_t Classify(std::string_view n, bool deferrable) {
  if (n.starts_with("vkCmd")) return deferrable ? kDefer : kDrain;
  static constexpr std::string_view kDrainNames[] = {
      "vkBeginCommandBuffer", "vkEndCommandBuffer", "vkResetCommandBuffer", "vkResetCommandPool",
      "vkAllocateCommandBuffers", "vkQueueSubmit", "vkQueueSubmit2", "vkQueueWaitIdle", "vkDeviceWaitIdle",
      "vkQueuePresentKHR", "vkUpdateDescriptorSets", "vkResetDescriptorPool"};
  for (auto k : kDrainNames)
    if (n == k) return kDrain;
  if (n.starts_with("vkDestroy") || n.starts_with("vkFree")) return kDrain;
  return kDirect;
}

template <size_t Id, typename PFN>
void Install(PFN real, PFN& target, const char* name) {
  using E = Wrapper<Id, PFN>;
  if (!real) return;
  E::real = real;
  E::mode = Classify(name, E::kDeferrable);
  if (E::mode != kDirect) target = &E::F;
}

Functions g_real;
Functions g_proxy;

// ---------------------------------------------------------------------------------------------------------------
// Calls with arrays or structures: their data is copied into the arena.
VkResult BeginCommands(VkCommandBuffer cb, const VkCommandBufferBeginInfo* info) {
  if (g_active.load(std::memory_order_relaxed) && !g_pinned_producer.load(std::memory_order_relaxed)) {
    REXLOG_INFO("[native] deferred recording: first command buffer, starting the worker");
    {
      s32 priority = 0;
      if (R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE))) g_priority_producer = int32_t(priority);
    }
    bool created = false;
    // A libnx thread, not std::thread: the ring thread is a Horizon thread created by the SDK, and a pthread
    // created from it hung the whole process.
    static Thread thread;
    if (R_SUCCEEDED(threadCreate(&thread, [](void*) { Worker(); }, nullptr, nullptr, 256 * 1024,
                                 g_priority_producer >= 0 ? g_priority_producer : 0x2C, -2)) &&
        R_SUCCEEDED(threadStart(&thread))) {
      created = true;
    }
    if (created) {
      t_producer = true;
      g_pinned_producer.store(true, std::memory_order_release);
      REXLOG_INFO("[native] deferred recording active: the thread that began the first command buffer queues, "
                  "a worker records");
    } else {
      g_active.store(false, std::memory_order_release);
      REXLOG_ERROR("[native] deferred recording DISABLED: could not create the worker");
    }
  }
  if (IsProducer()) DrainInternal();
  return g_real.vkBeginCommandBuffer(cb, info);
}

void Barrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm,
             const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni,
             const VkImageMemoryBarrier* im) {
  if (!IsProducer() || WithPNext(m, nm) || WithPNext(b, nb) || WithPNext(im, ni)) {
    if (IsProducer()) { DrainInternal(); ++g_direct; }
    g_real.vkCmdPipelineBarrier(cb, s, d, f, nm, m, nb, b, ni, im);
    return;
  }
  uint8_t* c = Arena(Size<VkMemoryBarrier>(nm) + Size<VkBufferMemoryBarrier>(nb) + Size<VkImageMemoryBarrier>(ni));
  const auto* m2 = Copy(c, m, nm);
  const auto* b2 = Copy(c, b, nb);
  const auto* i2 = Copy(c, im, ni);
  Enqueue(g_real.vkCmdPipelineBarrier, cb, s, d, f, nm, m2, nb, b2, ni, i2);
}

void BindSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n,
                 const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dynamic_offsets) {
  if (!IsProducer()) return g_real.vkCmdBindDescriptorSets(cb, bp, l, first, n, sets, nd, dynamic_offsets);
  uint8_t* c = Arena(Size<VkDescriptorSet>(n) + Size<uint32_t>(nd));
  const auto* s2 = Copy(c, sets, n);
  const auto* d2 = Copy(c, dynamic_offsets, nd);
  Enqueue(g_real.vkCmdBindDescriptorSets, cb, bp, l, first, n, s2, nd, d2);
}

void Constants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size,
                const void* data) {
  if (!IsProducer()) return g_real.vkCmdPushConstants(cb, l, st, off, size, data);
  uint8_t* c = Arena(size);
  const auto* d2 = Copy(c, static_cast<const uint8_t*>(data), size);
  Enqueue(g_real.vkCmdPushConstants, cb, l, st, off, size, static_cast<const void*>(d2));
}

void BeginPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* info, VkSubpassContents content) {
  if (!IsProducer() || info->pNext) {
    if (IsProducer()) { DrainInternal(); ++g_direct; }
    return g_real.vkCmdBeginRenderPass(cb, info, content);
  }
  uint8_t* c = Arena(Size<VkRenderPassBeginInfo>(1) + Size<VkClearValue>(info->clearValueCount));
  auto* i2 = const_cast<VkRenderPassBeginInfo*>(Copy(c, info, 1));
  i2->pClearValues = Copy(c, info->pClearValues, info->clearValueCount);
  Enqueue(g_real.vkCmdBeginRenderPass, cb, static_cast<const VkRenderPassBeginInfo*>(i2), content);
}

void Framings(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
  if (!IsProducer()) return g_real.vkCmdSetViewport(cb, first, n, v);
  uint8_t* c = Arena(Size<VkViewport>(n));
  Enqueue(g_real.vkCmdSetViewport, cb, first, n, Copy(c, v, n));
}
void Scissors(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
  if (!IsProducer()) return g_real.vkCmdSetScissor(cb, first, n, r);
  uint8_t* c = Arena(Size<VkRect2D>(n));
  Enqueue(g_real.vkCmdSetScissor, cb, first, n, Copy(c, r, n));
}
void ConstantsBlend(VkCommandBuffer cb, const float k[4]) {
  if (!IsProducer()) return g_real.vkCmdSetBlendConstants(cb, k);
  uint8_t* c = Arena(Size<float>(4));
  Enqueue(g_real.vkCmdSetBlendConstants, cb, Copy(c, k, 4));
}
void CopyBuffer(VkCommandBuffer cb, VkBuffer a, VkBuffer b, uint32_t n, const VkBufferCopy* r) {
  if (!IsProducer()) return g_real.vkCmdCopyBuffer(cb, a, b, n, r);
  uint8_t* c = Arena(Size<VkBufferCopy>(n));
  Enqueue(g_real.vkCmdCopyBuffer, cb, a, b, n, Copy(c, r, n));
}
void BufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n,
                  const VkBufferImageCopy* r) {
  if (!IsProducer()) return g_real.vkCmdCopyBufferToImage(cb, b, i, l, n, r);
  uint8_t* c = Arena(Size<VkBufferImageCopy>(n));
  Enqueue(g_real.vkCmdCopyBufferToImage, cb, b, i, l, n, Copy(c, r, n));
}
void ImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n,
                  const VkBufferImageCopy* r) {
  if (!IsProducer()) return g_real.vkCmdCopyImageToBuffer(cb, i, l, b, n, r);
  uint8_t* c = Arena(Size<VkBufferImageCopy>(n));
  Enqueue(g_real.vkCmdCopyImageToBuffer, cb, i, l, b, n, Copy(c, r, n));
}
void ClearAttached(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* a, uint32_t nr, const VkClearRect* r) {
  if (!IsProducer()) return g_real.vkCmdClearAttachments(cb, na, a, nr, r);
  uint8_t* c = Arena(Size<VkClearAttachment>(na) + Size<VkClearRect>(nr));
  const auto* a2 = Copy(c, a, na);
  const auto* r2 = Copy(c, r, nr);
  Enqueue(g_real.vkCmdClearAttachments, cb, na, a2, nr, r2);
}
void ClearColor(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* v, uint32_t n,
                 const VkImageSubresourceRange* r) {
  if (!IsProducer()) return g_real.vkCmdClearColorImage(cb, i, l, v, n, r);
  uint8_t* c = Arena(Size<VkClearColorValue>(1) + Size<VkImageSubresourceRange>(n));
  const auto* v2 = Copy(c, v, 1);
  const auto* r2 = Copy(c, r, n);
  Enqueue(g_real.vkCmdClearColorImage, cb, i, l, v2, n, r2);
}
void BindVertices(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o) {
  if (!IsProducer()) return g_real.vkCmdBindVertexBuffers(cb, first, n, b, o);
  uint8_t* c = Arena(Size<VkBuffer>(n) + Size<VkDeviceSize>(n));
  const auto* b2 = Copy(c, b, n);
  const auto* o2 = Copy(c, o, n);
  Enqueue(g_real.vkCmdBindVertexBuffers, cb, first, n, b2, o2);
}

// vkUpdateDescriptorSets queued (masseffect_deferred_native_update). Every array the call points to is copied into
// one arena block; the update then runs on the worker at the same position of the command stream, before the
// vkCmdBindDescriptorSets / draws recorded after it. Anything it cannot copy faithfully (pNext chains, a null array
// for a type that needs one, extension descriptor types) drains and runs on the ring thread as before.
void UpdateSets(VkDevice vulkan_device, uint32_t nw, const VkWriteDescriptorSet* w, uint32_t nc,
                    const VkCopyDescriptorSet* c) {
  if (!IsProducer()) return g_real.vkUpdateDescriptorSets(vulkan_device, nw, w, nc, c);
  size_t bytes = Size<VkWriteDescriptorSet>(nw) + Size<VkCopyDescriptorSet>(nc);
  bool simple = true;
  for (uint32_t i = 0; i < nw && simple; ++i) {
    const VkWriteDescriptorSet& e = w[i];
    if (e.pNext) {
      simple = false;
      break;
    }
    switch (e.descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        if (!e.pImageInfo) simple = false;
        else bytes += Size<VkDescriptorImageInfo>(e.descriptorCount);
        break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        if (!e.pTexelBufferView) simple = false;
        else bytes += Size<VkBufferView>(e.descriptorCount);
        break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
        if (!e.pBufferInfo) simple = false;
        else bytes += Size<VkDescriptorBufferInfo>(e.descriptorCount);
        break;
      default:
        simple = false;
        break;
    }
  }
  for (uint32_t i = 0; i < nc && simple; ++i)
    if (c[i].pNext) simple = false;
  if (!simple || bytes > (size_t(64) << 10)) {
    DrainInternal();
    ++g_direct;
    ++g_direct_updates;
    return g_real.vkUpdateDescriptorSets(vulkan_device, nw, w, nc, c);
  }
  uint8_t* cursor = Arena(bytes);
  auto* w2 = reinterpret_cast<VkWriteDescriptorSet*>(cursor);
  cursor += Size<VkWriteDescriptorSet>(nw);
  if (nw) std::memcpy(w2, w, sizeof(VkWriteDescriptorSet) * nw);
  const auto* c2 = reinterpret_cast<const VkCopyDescriptorSet*>(cursor);
  if (nc) std::memcpy(cursor, c, sizeof(VkCopyDescriptorSet) * nc);
  cursor += Size<VkCopyDescriptorSet>(nc);
  for (uint32_t i = 0; i < nw; ++i) {
    VkWriteDescriptorSet& e = w2[i];
    switch (e.descriptorType) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
        e.pImageInfo = Copy(cursor, e.pImageInfo, e.descriptorCount);
        break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        e.pTexelBufferView = Copy(cursor, e.pTexelBufferView, e.descriptorCount);
        break;
      default:
        e.pBufferInfo = Copy(cursor, e.pBufferInfo, e.descriptorCount);
        break;
    }
  }
  ++g_updates;
  Enqueue(g_real.vkUpdateDescriptorSets, vulkan_device, nw, static_cast<const VkWriteDescriptorSet*>(w2), nc,
          nc ? c2 : static_cast<const VkCopyDescriptorSet*>(nullptr));
}

// Functions the renderer fetches by name.
PFN_vkCmdCopyImage g_copy_image = nullptr;
PFN_vkCmdBlitImage g_blit = nullptr;
PFN_vkCmdClearDepthStencilImage g_clear_depth = nullptr;
PFN_vkCmdSetColorBlendEnableEXT g_active_blend = nullptr;
PFN_vkCmdSetColorBlendEquationEXT g_equation = nullptr;
PFN_vkCmdSetColorWriteMaskEXT g_mask = nullptr;
PFN_vkCmdInsertDebugUtilsLabelEXT g_label = nullptr;

void CopyImage(VkCommandBuffer cb, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n,
                  const VkImageCopy* r) {
  if (!IsProducer()) return g_copy_image(cb, a, la, b, lb, n, r);
  uint8_t* c = Arena(Size<VkImageCopy>(n));
  Enqueue(g_copy_image, cb, a, la, b, lb, n, Copy(c, r, n));
}
void Blit(VkCommandBuffer cb, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n,
          const VkImageBlit* r, VkFilter f) {
  if (!IsProducer()) return g_blit(cb, a, la, b, lb, n, r, f);
  uint8_t* c = Arena(Size<VkImageBlit>(n));
  Enqueue(g_blit, cb, a, la, b, lb, n, Copy(c, r, n), f);
}
void ClearDepth(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* v, uint32_t n,
                       const VkImageSubresourceRange* r) {
  if (!IsProducer()) return g_clear_depth(cb, i, l, v, n, r);
  uint8_t* c = Arena(Size<VkClearDepthStencilValue>(1) + Size<VkImageSubresourceRange>(n));
  const auto* v2 = Copy(c, v, 1);
  const auto* r2 = Copy(c, r, n);
  Enqueue(g_clear_depth, cb, i, l, v2, n, r2);
}
void ActiveBlend(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBool32* v) {
  if (!IsProducer()) return g_active_blend(cb, first, n, v);
  uint8_t* c = Arena(Size<VkBool32>(n));
  Enqueue(g_active_blend, cb, first, n, Copy(c, v, n));
}
void Equation(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorBlendEquationEXT* v) {
  if (!IsProducer()) return g_equation(cb, first, n, v);
  uint8_t* c = Arena(Size<VkColorBlendEquationEXT>(n));
  Enqueue(g_equation, cb, first, n, Copy(c, v, n));
}
void Mask(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkColorComponentFlags* v) {
  if (!IsProducer()) return g_mask(cb, first, n, v);
  uint8_t* c = Arena(Size<VkColorComponentFlags>(n));
  Enqueue(g_mask, cb, first, n, Copy(c, v, n));
}
void Label(VkCommandBuffer cb, const VkDebugUtilsLabelEXT* e) {
  if (!IsProducer() || e->pNext) {
    if (IsProducer()) { DrainInternal(); ++g_direct; }
    return g_label(cb, e);
  }
  const size_t long_value = e->pLabelName ? std::strlen(e->pLabelName) + 1 : 0;
  uint8_t* c = Arena(Size<VkDebugUtilsLabelEXT>(1) + Size<char>(uint32_t(long_value)));
  auto* e2 = const_cast<VkDebugUtilsLabelEXT*>(Copy(c, e, 1));
  if (long_value) e2->pLabelName = Copy(c, e->pLabelName, uint32_t(long_value));
  Enqueue(g_label, cb, static_cast<const VkDebugUtilsLabelEXT*>(e2));
}

template <size_t Id, typename PFN>
PFN_vkVoidFunction Generic(PFN_vkVoidFunction real) {
  using E = Wrapper<Id, PFN>;
  E::real = reinterpret_cast<PFN>(real);
  E::mode = E::kDeferrable ? kDefer : kDrain;
  return reinterpret_cast<PFN_vkVoidFunction>(&E::F);
}

}  // namespace

const Functions& Table(const Functions& real_fns) {
  if (!REXCVAR_GET(masseffect_native_deferred_recording)) return real_fns;
  static const bool built = [&] {
    g_real = real_fns;
    g_proxy = real_fns;
    const int32_t parts = REXCVAR_GET(masseffect_native_deferred_recording_parts);
    if (!(parts & 4)) {
#define XE_UI_VULKAN_FUNCTION(name) Install<__COUNTER__>(real_fns.name, g_proxy.name, #name);
#define XE_UI_VULKAN_FUNCTION_PROMOTED(extension_name, core_name) \
  Install<__COUNTER__>(real_fns.core_name, g_proxy.core_name, #core_name);
#include <rex/ui/vulkan/functions/device_1_0.inc>
#include <rex/ui/vulkan/functions/device_khr_swapchain.inc>
#include <rex/ui/vulkan/functions/device_1_1_khr_get_memory_requirements2.inc>
#include <rex/ui/vulkan/functions/device_1_1_khr_bind_memory2.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_maintenance4.inc>
#include <rex/ui/vulkan/functions/device_1_3_khr_dynamic_rendering.inc>
#undef XE_UI_VULKAN_FUNCTION_PROMOTED
#undef XE_UI_VULKAN_FUNCTION
    }
    if (!(parts & 8)) {
    // The calls with arrays: their own wrappers (the generic ones would only drain and call directly).
    if (!(parts & 16)) g_proxy.vkBeginCommandBuffer = &BeginCommands;
    g_no_enqueue = (parts & 32) != 0;
    g_proxy.vkCmdPipelineBarrier = &Barrier;
    g_proxy.vkCmdBindDescriptorSets = &BindSets;
    g_proxy.vkCmdPushConstants = &Constants;
    g_proxy.vkCmdBeginRenderPass = &BeginPass;
    g_proxy.vkCmdSetViewport = &Framings;
    g_proxy.vkCmdSetScissor = &Scissors;
    g_proxy.vkCmdSetBlendConstants = &ConstantsBlend;
    g_proxy.vkCmdCopyBuffer = &CopyBuffer;
    g_proxy.vkCmdCopyBufferToImage = &BufferToImage;
    g_proxy.vkCmdCopyImageToBuffer = &ImageToBuffer;
    g_proxy.vkCmdClearAttachments = &ClearAttached;
    g_proxy.vkCmdClearColorImage = &ClearColor;
    g_proxy.vkCmdBindVertexBuffers = &BindVertices;
    if (REXCVAR_GET(masseffect_deferred_native_update)) g_proxy.vkUpdateDescriptorSets = &UpdateSets;
    }
    g_fast = REXCVAR_GET(masseffect_deferred_native_fast);
    g_batch = g_fast ? uint32_t(REXCVAR_GET(masseffect_deferred_native_batch)) : 1;
    g_queue = new (std::nothrow) Slot[kSlots];
    g_arena = new (std::nothrow) uint8_t[kArena];
    if (!g_queue || !g_arena) {
      REXLOG_ERROR("[native] deferred recording DISABLED: out of memory for the queue ({} KB)",
                   (sizeof(Slot) * kSlots + kArena) / 1024);
      return false;
    }
    g_active.store(true, std::memory_order_release);
    REXLOG_INFO("[native] deferred recording ready: {} queue slots, {} MB arena", kSlots,
                kArena >> 20);
    return true;
  }();
  return built && (REXCVAR_GET(masseffect_native_deferred_recording_parts) & 1) ? g_proxy : real_fns;
}

PFN_vkVoidFunction Proc(PFN_vkGetDeviceProcAddr real, VkDevice device, const char* name) {
  const PFN_vkVoidFunction f = real(device, name);
  if (!f || !g_active.load(std::memory_order_acquire) ||
      !(REXCVAR_GET(masseffect_native_deferred_recording_parts) & 2))
    return f;
  const std::string_view n(name);
  if (n == "vkCmdCopyImage") { g_copy_image = reinterpret_cast<PFN_vkCmdCopyImage>(f); return reinterpret_cast<PFN_vkVoidFunction>(&CopyImage); }
  if (n == "vkCmdBlitImage") { g_blit = reinterpret_cast<PFN_vkCmdBlitImage>(f); return reinterpret_cast<PFN_vkVoidFunction>(&Blit); }
  if (n == "vkCmdClearDepthStencilImage") { g_clear_depth = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(f); return reinterpret_cast<PFN_vkVoidFunction>(&ClearDepth); }
  if (n == "vkCmdSetColorBlendEnableEXT") { g_active_blend = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(f); return reinterpret_cast<PFN_vkVoidFunction>(&ActiveBlend); }
  if (n == "vkCmdSetColorBlendEquationEXT") { g_equation = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(f); return reinterpret_cast<PFN_vkVoidFunction>(&Equation); }
  if (n == "vkCmdSetColorWriteMaskEXT") { g_mask = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(f); return reinterpret_cast<PFN_vkVoidFunction>(&Mask); }
  if (n == "vkCmdInsertDebugUtilsLabelEXT") { g_label = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(f); return reinterpret_cast<PFN_vkVoidFunction>(&Label); }
#define GENERIC(fn_name) \
  if (n == #fn_name) return Generic<__COUNTER__, PFN_##fn_name>(f);
  GENERIC(vkCmdWriteTimestamp)
  GENERIC(vkCmdSetCullMode)
  GENERIC(vkCmdSetFrontFace)
  GENERIC(vkCmdSetPrimitiveTopology)
  GENERIC(vkCmdSetPrimitiveRestartEnable)
  GENERIC(vkCmdSetDepthBiasEnable)
  GENERIC(vkCmdSetDepthTestEnable)
  GENERIC(vkCmdSetDepthWriteEnable)
  GENERIC(vkCmdSetDepthCompareOp)
  GENERIC(vkCmdSetStencilTestEnable)
  GENERIC(vkCmdSetStencilOp)
  GENERIC(vkGetQueryPoolResults)
#undef GENERIC
  if (n.starts_with("vkCmd")) {
    // A recording call this proxy does not know would run out of order: no deferral at all.
    g_active.store(false, std::memory_order_release);
    REXLOG_ERROR("[native] deferred recording DISABLED: no wrapper for {}", name);
  }
  return f;
}

void Drain() {
  if (IsProducer()) DrainInternal();
}

void Flush() {
  if (g_fast && g_published != g_written_producer && IsProducer()) PublishAlready();
}

}  // namespace masseffect::native::deferred
