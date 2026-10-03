// masseffect - native renderer: draws with the native shader library (see masseffect_native_draws.h).
//
// What it covers
//   - Shaders: modules of the native shader library with the XenosRecomp interface
//     (shader_common.h and shader_recompiler.cpp): push constants with three
//     buffer addresses (VS, PS and shared constants), 2D, 3D and cubemap
//     textures in sets 0-2 and samplers in set 3, with no size limit;
//     specialization constant 0 with R11G11B10 normals (bit 0) and alpha test (bit 1).
//   - Vertices: the input comes from the fetches of the VS patched by D3D, each
//     element found by its destination register. Guest data is converted
//     to host words with the fetch constant's byte order, like the emulation
//     (spirv_translator_fetch.cpp), and uploaded to the frame buffer.
//   - Indices: VGT_DMA_BASE/SIZE or automatic, with VGT_INDX_OFFSET. Quad lists
//     become triangles v0 v1 v2 / v0 v2 v3 (primitive_processor.cpp).
//   - 2D textures by fetch constant: linear or tiled, formats 8, 8_8, 8888,
//     2_10_10_10, 16, 16_16, 16F, 32F, DXT1/3/5, DXT5A and DXN, with the fetch
//     constant's swizzle in the view. The resolved textures are sampled as they
//     are. The content is compared once per frame. The game's mip levels are
//     included (masseffect_native_mipmaps), packed as on the Xbox 360; without them,
//     foliage and ground textures looked grainy in the distance.
//   - State: blending, color mask, depth, stencil, culling, viewport (without the
//     half pixel: the shader adds it, g_HalfPixelOffset) and window scissor.
//   - Clipping disabled (draws in pixels, like the videos): viewport the size of
//     the render target and the transform in the VS with g_NdcScale/g_NdcOffset,
//     which only the library regenerated with the MASSEFFECT XenosRecomp has.
//
// Not covered (rejected or substituted, with the cause logged once)
//   Mips of 3D textures, vertex textures, signed or gamma textures, points,
//   rectangle lists, line loops and vertex formats without a direct Vulkan
//   equivalent.

#include "masseffect_deferred_recording.h"
#include "masseffect_native_draws.h"
#include "me_formats_color.h"
#include "masseffect_waits_hitch.h"

#include "masseffect_native_vertices_dedupe.h"
#include "me_primitives.h"
#include "me_depth.h"
#include "me_rectangle_spirv.h"
#include "me_depth_spirv.h"
#include "me_depth_quantize_spirv.h"
#include "me_fragcoord_xy_spirv.h"
#include "me_edram_ownership.h"
#include "me_raster_state.h"
#include "../me_vertex_fetch_selection.h"
#include "masseffect_native_textures_pool.h"

#include "masseffect_native_shaders.h"
#include "masseffect_native_hooks.h"  // ActiveLibrary
#include "masseffect_shader_library.h"

#include <condition_variable>
#include <deque>
#include <rex/cvar.h>
#include <rex/filesystem.h>

#include <rex/graphics/registers.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_INT32(masseffect_native_float24_ps_mode, 0, "Mass Effect",
                    "Experimental MODE4 half-range D24FS8 fragment depth quantization: "
                    "0 original D32 precision, 1 truncate, 2 round-even. Includes explicit depth "
                    "and synthetic depth-only fragment stages. Not GPU conformance.").range(0, 2);
REXCVAR_DEFINE_INT32(masseffect_native_diag_msaa2_phase, 2, "Mass Effect",
                    "Diagnostic collapsed-2x raster phase: 0 unchanged, 1 SDK native-2x sample0, "
                    "2 measured ME viewport jitter cancellation. Clipped XY-transformed draws only, "
                    "remaps supported material FragCoord XY inversely. NOT complete MSAA or a default correction.").range(0, 2);
REXCVAR_DEFINE_BOOL(masseffect_native_gpu_labels, false, "Mass Effect",
                    "Diagnostic GPU draw labels: shader fingerprints, target and draw number");
REXCVAR_DEFINE_BOOL(masseffect_native_sort_updates_texture, true, "Mass Effect",
                    "Submit earlier draws before replacing an already uploaded texture's texels");

#include <arm_neon.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <fstream>
#include <set>
#include <string>
#include <system_error>  // the bind thread
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). The method it picks for GCC (1) reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the write of the data
 * being hashed (strict aliasing). The texture key read keys[4] before writing it, and the same texture
 * was created several times. Same hash values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would take effect too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "masseffect_crc_fingerprint.h"

// Only for RexSwitchSetCurrentThreadPriorityOk (texture bind thread). That header deliberately does not
// include switch.h (same as in the native graphics system).
#include <threading_switch.h>

/*
 * p_000139, the final composite, is a single full-screen quad with twelve texture samples: 2.8-3.2 real
 * ms, 72-80 % of all post-processing. Seven of those twelve are the radial speed blur. Turning it off
 * removes them and leaves the center tap.
 */
/*
 * Vegetation in the shadow map.
 *
 * The four alpha-tested pixel shaders of the shadow pass (n36, n68, n99, n103) are trees, bushes and wire
 * fences. Measured on PC with an A/B test: they are 55 % of the pass's draws but only 12 % of its
 * triangles.
 *
 * The pass cost follows the triangle count, and removing them is worth -1.2 ms of GPU but -3.1 ms of CPU,
 * which is also at 96 % of a core.
 *
 * The right constant is 0.556 real ms per 10,000 triangles, not the 0.80 first estimated (fit over 17
 * intervals with a constant open area, r2 = 0.982). With it, 12 % of
 * the triangles would be -0.44 ms, so about -0.76 ms of the -1.2 ms measured here is not geometry: it is
 * the vegetation's alpha pixel shader, which in the shadow map runs in full only to produce the cutout.
 *
 * What is lost: trees and fences stop casting shadows. Everything else keeps its shadow. It shows
 * wherever there is dense foliage.
 */
/*
 * Enabled by default. It used to be enabled only by a line in a local toml, and the toml overrides the
 * default: shipping without that line would silently lose the improvement. Three cvars were found in
 * that state; one of them was worth 2 ms and had been inactive for six builds.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_tile_fast, true, "MASSEFFECT",
                    "Native renderer: texture untiling in 16-byte steps, with the row "
                    "computation done once. The first 2000 levels are checked against the regular path "
                    "and, if one differs, the fast path switches itself off. false = regular block-by-block path");

/*
 * The FramebufferFor cache forgets destroyed views. It is keyed by handle values (render pass, 5 views
 * and size) and used to release nothing until the destructor. When the render target code destroys an image (GetResolved
 * recreates it with another size or format at the same address, and those textures end up as render
 * targets through image swaps), its framebuffers stayed behind and, if Vulkan gave the same handle to a
 * new view, a framebuffer created on the dead view was returned. Destroy now notifies through
 * ForgetView and the framebuffers that use the view are destroyed. false = previous behaviour (nothing
 * is forgotten).
 */
REXCVAR_DEFINE_BOOL(masseffect_native_framebuffers_forget_views, true, "MASSEFFECT",
                    "Native renderer: when a C2 image is destroyed, the "
                    "framebuffers that use its view are destroyed too, so a reused handle cannot return a stale one. false = "
                    "previous behavior");

REXCVAR_DEFINE_INT32(masseffect_native_fingerprints_kb_frame, 6144, "MASSEFFECT",
                     "Native renderer: KB of STABLE textures the ring may recheck at most "
                     "in one frame; those that do not fit wait 1-2 frames (at most 8 times "
                     "in a row). Spreads out rechecks that used to land in the same frame. "
                     "0 = no limit, as before");

REXCVAR_DEFINE_INT32(masseffect_native_texture_interval_max, 32, "Mass Effect",
                     "Longest interval, in frames, between guest-memory rechecks of a stable texture (default 32, "
                     "plus 0-7 jitter). A game that streams new texel data into an already-sampled address (texture "
                     "pool reuse) shows the old content for up to this many frames, which looks like texture "
                     "popping; lower it (8, 4) to trade ring CPU time for less popping. Must be a power of two.")
    .range(2, 32);

REXCVAR_DEFINE_BOOL(masseffect_native_adaptive_texture, true, "Mass Effect",
                    "A texture whose guest memory was seen to change after it had looked stable (streaming into a "
                    "reused address, animated lookup tables) never goes back to a long recheck interval: its maximum "
                    "becomes 4 frames after the first late change and 2 after the third. Fixes textures that jump "
                    "every ~32 frames; costs a few extra hashes only for those textures. false = every texture "
                    "backs off to masseffect_native_texture_interval_max again");

/*
 * Sampled recheck of stable textures. See PrepareTexture and SampleFingerprint.
 * Enabled by default: its guard verifies itself and switches off at the first mismatch.
 */
REXCVAR_DEFINE_INT32(masseffect_native_fingerprints_sampling, 8, "MASSEFFECT",
                     "Native renderer: STABLE textures are rechecked with the fingerprint of "
                     "a sample (first and last 4 KB block plus 1 in 8, base and mips) instead of all their "
                     "bytes; if the sample changes, the full path is taken. 1 in N rechecks of "
                     "each texture is still a full one, and the first 3000 compute both fingerprints: a single "
                     "disagreement switches the sampling off. 0 = always the full fingerprint, as before")
    .range(0, 64);

REXCVAR_DEFINE_BOOL(masseffect_shadows_no_vegetation, false, "MASSEFFECT",
                    "Do not draw alpha-tested geometry into the shadow map: trees, bushes and "
                    "wire fences. They are 55 % of the pass draws and 12 % of its triangles, "
                    "so this saves little GPU time (-1.2 ms) and quite a lot of CPU time (-3.1 ms). Trees stop "
                    "casting shadows");

// Enabled by default. On the console (A and B alternating every 30 s), the UBO intervals run 18-23 %
// faster than the neighbouring pointer intervals at the same draw load.
REXCVAR_DEFINE_BOOL(masseffect_native_constants_ubo, true, "MASSEFFECT",
                    "Native renderer: shaders read their constants from dynamic UBOs (constant bank on "
                    "Maxwell) instead of through a 64-bit pointer. Same bytes: the image does not change. Needs the "
                    "shader library built with SPEC_CONSTANT_CONSTANTS_UBO; false goes back to the pointer");
/*
 * Descriptor set 4 by differences (the work is done in NVK, see mesa/mesa-switch-masseffect.patch).
 *
 * vkCmdBindDescriptorSets for set 4 costs 1.3-1.4 us per call in play and happens in 70-78 % of the
 * draws ("C6 substages" report), and it leaves four cbuf rebinds for the Draw. Almost all of it is NVK:
 * four writes to the root table (one per word of the dynamic descriptors, even when only the low part of
 * the address changes) and an 80-slot walk that dirties every cbuf of the set even when only one offset
 * changed. With the patch, NVK sends only the words that change and rebinds only the cbufs whose
 * descriptor changes. It is exact (the GPU ends up with the same bindings), and NVK checks it against
 * what it really bound in each slot: on a MISMATCH it fixes that draw, switches off for the session and
 * ControlSet4 reports it in the log as an error.
 * This cvar requests or withdraws it from NVK on each submission; with an unpatched Mesa (or on PC) it
 * does nothing. false = NVK binds the whole set, as usual.
 */
// Enabled by default. Measured: 1 root-table write per bind instead of 4, although the set 4 bind still
// costs 1.31-1.45 us (1.29-1.63 before). At one point the NVK guard seemed to check no cbuf at all; that
// was a bug in its own sampling ("1 in 4,096 draws of the buffer", and no buffer reaches 4,096 draws),
// not a sign that there was nothing to check. In its first ~40 s it checked 386,271 cbufs without a
// single difference, and the Draw rebound 38-45 % fewer cbufs. The patched Mesa samples with the process-wide count.
REXCVAR_DEFINE_BOOL(masseffect_native_set4_differences, true, "MASSEFFECT",
                    "Native renderer: descriptor set 4 (UBO constants) by differences in "
                    "NVK: only the descriptors that change are sent and rebound. Same result (self-checked "
                    "in NVK). Needs the Mesa build with patch_nvk_set4; false = as usual");
/*
 * The draw path in NVK (patched Mesa, mesa/mesa-switch-masseffect.patch).
 *
 * The ring is the bottleneck, and ~3.4 us of each draw is spent inside NVK: vkCmdDrawIndexed 1.4-1.6 us
 * per call, vkCmdBindPipeline 3-3.7 and set 4 1.3 ("C6 substages" report). Four exact improvements (the
 * GPU receives the same commands with the same data), each with its own guard in NVK that compares
 * against the usual path for the first 20,000 uses and then 1 in 1,024, and switches off on a MISMATCH:
 *   - emission: each command written in one go (Draw, cbuf rebinds, root table, BIND_VB), same bytes;
 *   - cbufs: only the slots that can get dirty, and no cbuf flush when nothing is dirty;
 *   - dynamic: only the dynamic state groups with dirty bits;
 *   - prefetch: cache hints (PRFM) for the pipeline and shaders before they are used; the app also hands
 *     the pipeline to NVK as soon as it knows it (after PipelineFor), several us before vkCmdBindPipeline.
 * Estimated for the four together: 0.6-1.5 us per draw (~1.1-2.8 ms of ring time per frame). The
 * per-part measurement (CNTPCT clock, 1 in N calls) gives the real breakdown in "C6 NVK parts".
 * With an unpatched Mesa (or on PC) none of this does anything.
 */
REXCVAR_DEFINE_INT32(masseffect_native_nvk_measure, 64, "MASSEFFECT",
                     "Native renderer: measures by parts inside NVK on 1 in N calls "
                     "(power of 2; 'C6 NVK parts' report every 10 s). 0 = no measuring")
    .range(0, 4096);
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_measure_failures, false, "MASSEFFECT",
                    "Native renderer: reads ahead what the pipeline bind and the shader "
                    "flush will use and times it separately (how much is cache misses). Does extra work: for "
                    "measuring only");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_emission, true, "MASSEFFECT",
                    "Native renderer: NVK writes each draw command in one go, with the "
                    "same bytes (self-checked in NVK). false = as usual");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_cbufs, true, "MASSEFFECT",
                    "Native renderer: NVK only looks at the cbuf slots that can get dirty "
                    "and skips the cbuf flush when nothing is dirty (self-checked in NVK). false = as usual");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_dynamic, true, "MASSEFFECT",
                    "Native renderer: NVK only emits the dynamic state groups with "
                    "dirty bits (self-checked in NVK). false = as usual");
REXCVAR_DEFINE_BOOL(masseffect_native_nvk_preload, true, "MASSEFFECT",
                    "Native renderer: cache hints (PRFM) for the pipeline and shaders before "
                    "they are used, and the pipeline handed to NVK after PipelineFor. Changes nothing visible. false = no hints");

/*
 * The state of set 4 by differences lives in NVK (struct nvk_switch_set4 in nvk_cmd_buffer.h,
 * mesa/mesa-switch-masseffect.patch). Same field order and types, with a version: it is a contract. Weak symbol:
 * with an unpatched Mesa the address is null and ControlSet4 reports it once.
 */
extern "C" {
struct NvkSwitchSet4Counts {
  uint64_t bindings_difference;
  uint64_t complete_bindings;
  uint64_t writes_root;
  uint64_t dwords_root;
  uint64_t cbufs_dirty;
  uint64_t cbufs_saved;
  uint64_t draws;
  uint64_t checks;
  uint64_t differences;
};
struct NvkSwitchSet4 {
  int32_t version;
  int32_t request;
  int32_t environment;
  int32_t off;
  NvkSwitchSet4Counts total;
};
extern NvkSwitchSet4 nvk_switch_set4 __attribute__((weak));
}
static_assert(sizeof(NvkSwitchSet4) == 16 + 9 * 8, "NvkSwitchSet4 must have the same size as in NVK");
/*
 * The draw path in NVK (struct nvk_switch_draw in nvk_cmd_buffer.h, patched Mesa). Same
 * field order and types, with a version: it is a contract. Weak symbols: with an unpatched Mesa the
 * addresses are null and ControlDrawNvk reports it once.
 */
extern "C" {
struct NvkSwPart {
  uint64_t times;
  uint64_t ticks;  // CNTPCT_EL0, a ticks_per_second
};
struct NvkSwImprovement {
  int32_t request;  // written by the app: 1 yes, 0 no
  int32_t off;  // 1: its guard saw a MISMATCH
  uint64_t uses;
  uint64_t validated;
  uint64_t differences;
  uint64_t no_check;
};
struct NvkSwitchDraw {
  int32_t version;
  int32_t measure;
  int32_t measure_failures;
  int32_t environment;
  uint64_t ticks_per_second;
  NvkSwPart parts[16];   // enum nvk_sw_part
  uint64_t counts[13];    // enum nvk_sw_count
  NvkSwImprovement improvements[5];  // emission, cbufs, dynamic, set4_fast (not applied), prefetch
};
extern NvkSwitchDraw nvk_switch_draw __attribute__((weak));
void vk_switch_preload_pipeline(VkPipeline pipeline) __attribute__((weak));
}
static_assert(sizeof(NvkSwitchDraw) == 24 + 16 * 16 + 13 * 8 + 5 * 40, "NvkSwitchDraw: same size as in NVK");
/*
 * Sampler caches valid across frames.
 *
 * With three work slots, this cache once turned a text overlay that the game redraws every frame into a smear;
 * with two slots it did not show, and disabling the cache with three slots made the text perfect again.
 *
 * The cause: two paths set no validity horizon at all. Resolved textures (a render target read back as a
 * texture) set `valid_until = UINT64_MAX`, "valid forever while the generation does not change", and
 * such an overlay is exactly that: a render target the game rewrites every frame. With three work
 * slots the CPU runs two frames ahead and the descriptor slot of an old view was reused, hence the smear.
 * With two slots the distance was not enough for it to show.
 *
 * Fixed by setting `valid_until = frame_` on those two paths (see the comments in PrepareTexture).
 * The other paths were already tied to `texture.next`, the same policy that decides when the content
 * is rechecked: nothing that was there before is relaxed.
 *
 * What it buys: the cold path of the `textures` stage costs 13.5 us and runs 266 times per frame =
 * 3.6 ms. The hot path (2,893 hits) is indistinguishable from zero. With the cache valid across frames,
 * the cold ones drop to those expiring in that frame (~45-70), i.e. 2.6-3.0 ms.
 *
 * What to check, in motion (not paused): the HUD and the text overlays. If anything looks blurry or delayed, disable this cvar in the toml.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_cache_textures_between_frames, true, "MASSEFFECT",
                    "Native renderer: sampler caches stay valid across frames as long as it is not time to "
                    "recheck the texture contents. false: they expire every frame, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Uploads the game's mip levels. With only the base level of each texture, distant surfaces looked grainy
// compared with the Xbox 360. This also fixes the base level of small textures with packed mips, which
// does not start at the base address.
REXCVAR_DEFINE_BOOL(masseffect_native_mipmaps, true, "MASSEFFECT",
                    "Native renderer: uploads the mip levels that the game provides (as on the Xbox 360). false: base "
                    "level only, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// A mip level read from the wrong place (packed tail offset, row or slice alignment) causes no Vulkan
// errors, only smudges in the distance. The game's mips are reductions of the base level, so their
// average color has to resemble the base level's.
// The texture cache used to release nothing. On the console it went from 62 MB at the start of
// a session to 392 MB after 12 minutes without levelling off (2,578 textures); with mipmaps it grows 25 %
// faster. Above the limit, the textures unused for the longest are evicted.
// 384 MB was once too much for the console: the cache reached 313 MB after 14 minutes and the GPU ran out
// of memory before getting near the limit: nvMapCreate failed even for 64 KB and the screen went black
// with the audio still playing. The limit was then lowered to 192 MB, to leave room for the render
// targets, guest memory and the rest.
/*
 * 192 -> 384, based on the GPU memory budget report.
 *
 * On the console: "heap 0 (GPU): 478 MB used of 1375 MB budgeted (size 2391 MB); the texture cache
 * holds 150 MB of 192". So more does fit: almost 900 MB of the budget is untouched.
 *
 * And the 192 limit was doing harm: "texture cache near the limit: cold textures dripping out (200
 * in total)" in every report, with 53 MB of texture uploads every 10 s. That is evicting textures only
 * to upload them again right away, and each re-upload is a spike on the ring thread. It matches the
 * stutters: 3.5-5 % of the frames exceed 50 ms.
 *
 * The reason for lowering it to 192 still stands (the GPU ran out of memory after 14 minutes with the
 * cache at 313 MB), but that was with 384 and without the emergency path that exists now (stop the GPU,
 * release half the cache and retry). With 478 of 1375 MB used, 384 leaves a 700 MB margin.
 */
/* 384 -> 512 on the Switch, with masseffect_internal_resolution = "automatic" (720p handheld and 1080p docked).
 * In play at 1024x576 the cache already reached 272-281 MB, and at 720p and 1080p the resolved render targets
 * are larger, so the resolution and the limit go up together: raising only the resolution once filled the cache,
 * and releasing textures to upload them again caused stutters. GPU memory on the console: 514 MB used of
 * 1,492 MB budgeted. masseffect.toml has the same value. */
constexpr int32_t kTexturesMbMaxByDefault = 512;
REXCVAR_DEFINE_INT32(masseffect_native_textures_mb_max, kTexturesMbMaxByDefault, "MASSEFFECT",
                     "Native renderer: MB of textures above which those unused for the longest time are released "
                     "(at least 120 frames), down to 75 %. If the game asks for them again, they are uploaded "
                     "again. 0 = no limit, as before")
    .range(0, 4096)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Creating a new texture without stalling the ring thread.
 *
 * With the memory pool, creating a texture no longer asks the system for memory, but vkBindImageMemory
 * still makes two ioctls: the plane's VA with its pte_kind and the mapping of the pool chunk
 * (nvk_image.c:1696-1710; all our textures are tiled with pte_kind GENERIC_16BX2,
 * nil/image.rs:439-440). In play that is ~0.6 ms of wall time per texture, and entering a new zone
 * brings 33-45 at once: 19-27 ms of stalled ring in that frame.
 * With this, the ring does the CPU part and the bind thread does vkBindImageMemory while the ring carries
 * on with the draws; before closing the upload buffer it waits for whatever is missing and records the
 * barrier and the copy in that same buffer. The GPU receives the same thing in the same submission: no
 * placeholder textures and no lower mips.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_textures_binding_thread, true, "MASSEFFECT",
                    "Native renderer: the vkBindImageMemory of new textures (address reservation and mapping on Horizon, ~0.6 ms each) "
                    "runs on a separate thread while the ring keeps recording; "
                    "whatever is still missing is awaited before submitting. Same result (self-checked). false = on the ring")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_textures_binding_thread_priority, 0x2D, "MASSEFFECT",
                     "Native renderer: priority of the texture-binding thread. 0x2D, the ring's: "
                     "above the guest (0x3B) so a finished ioctl does not wait for a core, and outside the "
                     "audio (0x2B) and presentation (0x2C) slots. It sleeps in the ioctl almost all the time")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Measurement only: new textures whose content repeats a live one. See NoteContentTexture. It changes
 * no decision of the ring: it only counts, and reports every 10 s in the "C3 reuse by content"
 * line.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_diag_reuse, true, "MASSEFFECT",
                    "Native renderer (measurement only): counts how many new textures have the "
                    "same content and shape as another live one in the cache (the game reloads packs at another address) "
                    "and how many of those other ones have gone unused for more than 120 frames. Changes nothing. false = no "
                    "counting")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Pipeline prewarming. A pipeline that is not in the Vulkan cache is compiled on the fly on the ring: 68-159 ms
 * each on the console (56 of them in the first run, 5.2 s of stutter). It happens the first time after any
 * change to the shader library, the driver or the key, and on a fresh install. See PrewarmedLoop.
 */
REXCVAR_DEFINE_BOOL(masseffect_shaders_preload, true, "Mass Effect",
                    "With the shader index (masseffect_shaders.mesp.idx, SPIR-V read on first use), a background "
                    "thread reads at start-up the SPIR-V of every shader in the pipeline prewarm list "
                    "(cache/masseffect_native_pipelines.bin), so the ring does not read the SD card when it first uses them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_pipelines_prewarm, false, "MASSEFFECT",
                    "Native renderer: at start-up, a lowest-priority thread re-creates "
                    "in the Vulkan cache the pipelines the ring created in earlier sessions "
                    "(its list is in cache/masseffect_native_pipelines.bin), with the same function as the ring, and destroys them: when the "
                    "ring requests them they are already compiled (no 70-160 ms hitches per pipeline on the first "
                    "run after a change). Changes no pipeline and no draw. false = no prewarming (the "
                    "list is still saved)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * This test has been answered: there is nothing to gain.
 *
 * A fit over 17 intervals gives, for the shadow pass, raw ms = 0.0342 x thousands of triangles + 0.023
 * (r2 = 0.982). The intercept is 0.037 real ms: with both 1600x1600 maps open (5.12 Mtexels, i.e. 20.5 MB of
 * loadOp = LOAD per frame) and zero triangles, the pass costs nothing measurable. So on this GPU the
 * loadOp = LOAD of a depth attachment is not paid for: Maxwell does not load the tile up front like a tiled
 * GPU, it reads on demand. The theory of "130-165 MB per frame moved for nothing" further below does not
 * hold for the shadow map depth.
 *
 * On ZCULL: in NVK (nvk_cmd_draw.c, `use_zcull`) a pass with loadOp = CLEAR enables ZCULL even when the
 * image has no plane (ephemeral, without LOAD/STORE between passes, which a map drawn whole every time does
 * not need), while DONT_CARE, which this cvar uses, does not: the condition is
 * `zcull_plane || loadOp == CLEAR`, and DONT_CARE is neither. CLEAR has the same visual risk as DONT_CARE and
 * leaves the map at 1.0 (far) instead of garbage, so it is the better of the two. It still does not pay off:
 * the ZCULL ceiling here is those 0.037 ms, because the pass is pure geometry. For ZCULL to cover the map,
 * the renderArea has to span the full 1600x1600: the ZCULL region comes from render->area, not from the
 * image size.
 */
/*
 * Enabled by default. It used to be enabled only by a line in a local toml, and the toml overrides the
 * default: shipping without that line would silently lose the improvement. Three cvars were found in
 * that state; one of them was worth 2 ms and had been inactive for six builds.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_pass_narrow_dependency, true, "Mass Effect",
                    "Native renderer: the draw render pass's start dependency orders only attachment and transfer "
                    "writes (no MEMORY_WRITE: on NVK that flushes L1 and shader caches at every pass begin)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_vs_pruned_outputs, true, "Mass Effect",
                    "Vertex shaders (package v30+) write only the outputs the pipeline's pixel shader reads "
                    "(specialization constant 60). false = all 22 outputs, as before");
// NVK patch: read as NVK_SWITCH_STAGE_CACHE before the device is created.
REXCVAR_DEFINE_BOOL(masseffect_nvk_cache_per_stage, false, "Mass Effect",
                    "NVK: per-stage shader cache keys (runtime disable_lto), so a VS or PS already compiled for "
                    "another pipeline is not compiled again; needs the patched driver");
REXCVAR_DEFINE_STRING(masseffect_diag_trace_tile, "", "Mass Effect",
                      "Diagnostic: physical EDRAM tile (0-2047) whose ownership/sync events are logged "
                      "(sets MASSEFFECT_EDRAM_TRACE_TILE before the renderer starts)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_normalized_remap, true, "Mass Effect",
                    "Vertex input remap codes: slots that do not change their component are written as 'unchanged' "
                    "(7), so the vertex shader's identity fast path skips the per-vertex select chain (exact)");
REXCVAR_DEFINE_BOOL(masseffect_cold_startup, false, "Mass Effect",
                    "Cold-start test: no Mesa shader disk cache (MESA_SHADER_CACHE_DISABLE) and the pipeline cache / "
                    "prewarm list from cache/cold.bin (deleted by the test cycle) instead of the normal file")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_pass_shadows_no_load, true, "MASSEFFECT",
                    "Native renderer: opens the shadow map pass without loading its "
                    "previous contents (loadOp = DONT_CARE). Measured in build 112: nothing to gain, the "
                    "depth loadOp is not paid for on this GPU (the pass with 0 triangles costs "
                    "0.037 ms)");
REXCVAR_DEFINE_BOOL(masseffect_native_diag_vertices_repeated, false, "MASSEFFECT",
                    "Native renderer: counts the vertex bytes that repeat "
                    "address, size and contents within the same frame or against an earlier one");
// Mass Effect rewrites transient UI and movie vertex buffers while the native ring is still consuming the
// preceding draws. Deferring the read of guest memory to the copy thread races those rewrites:
// letters disappear and arbitrary triangles appear from frame to frame. Keep the safe synchronous path as the
// default for this executable. The option remains available for controlled profiling once the copy queue owns a
// snapshot of the source bytes rather than a borrowed guest pointer.
REXCVAR_DEFINE_BOOL(masseffect_native_uploads_thread, false, "MASSEFFECT",
                    "Native renderer: copies vertices on a separate thread. On Mass Effect it is experimental: "
                    "dynamic buffers can be rewritten before the thread reads them; false keeps "
                    "a coherent snapshot on the ring thread")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The ring no longer waits for a copy thread that has no core (see WaitUploads).
 *
 * In play, the ring waited 14.7 ms for the copy thread during one stutter (3,965 draws) and 17.9 ms in
 * the next one, and the game spent 21.1 ms without room in the ring. The thread ran at 0x3B, below the two
 * game threads (0x3A), and with the CPU at 278 % out of 300 it sat ready without a core: of those ms, only
 * ~6-7 were copying.
 */
/*
 * Copy thread priority. It used to run at its creation priority (0x3B, below the two game threads at 0x3A)
 * and, with the ring's help path (masseffect_native_uploads_help), the ring always reached the copies first: the
 * thread copied 0 MB and the ring 100 % (1.4-1.8 GB, ~1 s every 10 s: ~3 ms per frame on the thread that is
 * the bottleneck). At 0x2E it takes the copies as soon as they are queued, on another
 * core; the ring (0x2D) preempts it if they share a core, and the help path only handles what is left when
 * waiting.
 */
REXCVAR_DEFINE_INT32(masseffect_native_uploads_thread_priority, 0x2E, "MASSEFFECT",
                     "Native renderer: priority of the vertex-copy thread. 0x2E: below "
                     "the ring (0x2D) and above the guest (0x3A-0x3B), so it copies while the "
                     "ring records. 0x3B = as up to build 186")
    .range(0x2C, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * The copy thread on a different core from the ring. In a busy scene the thread only did 20 % of
 * the copies: the ring, with higher priority (0x2D) and at 94 %, did not leave it its core, and did them
 * itself while waiting (2.5-5 ms per frame). With its preferred core elsewhere, the thread takes CPU from the
 * game threads (0x3A, lower priority), which spend 97 % of their time waiting for the ring. Only the preferred
 * core is set: the affinity mask is untouched and the kernel can still move it.
 */
REXCVAR_DEFINE_INT32(masseffect_native_uploads_thread_core, -2, "MASSEFFECT",
                     "Native renderer: preferred core of the vertex-copy thread. -2 = one "
                     "different from the ring's; -1 = no preference (as up to build 190); 0-2 = that core")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * 16-bit indices with hand-written NEON (IndicesFrom16). At one point the compiler stopped vectorizing that
 * loop (0 vector rev16 in Draw, 7 before) and the "indices" substage rose to 2.95 us per draw (the whole
 * stage had been 1.2-1.3 us): ~3 ms of ring time per frame. Same results (the same indices in the same order,
 * the same minimum and maximum). Guard: the first 20,000 draws, then 1 in 4,096, are compared with the plain
 * loop; on a MISMATCH the plain loop is kept for the session.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_indices_neon, true, "MASSEFFECT",
                    "Native renderer: 16-bit indices are copied and measured (minimum and "
                    "maximum) with NEON, 16 per iteration. Same result (self-checked). false = the plain loop")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_uploads_help, true, "MASSEFFECT",
                    "Native renderer: when the ring thread has to wait for vertex copies, "
                    "it copies by itself those the copy thread has not started yet and waits for the one in "
                    "progress with priority inheritance. Same data (self-checked). false = waits as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(masseffect_native_inv_tex_size, true, "MASSEFFECT",
                    "Native renderer: shaders take 1/texture size from the "
                    "constants instead of asking the texture on every offset sample. It is "
                    "the same computation with the same number: the image does not change. Needs a "
                    "shader library regenerated with the new helper");
/*
 * Enabled by default: a single texel instead of the 3x3, measured and long enabled in a local toml. Three
 * cvars were only enabled through a local toml, and the toml overrides the default: shipping without that
 * line would lose the improvement without anyone noticing.
 */
/*
 * The cheap PCF was briefly disabled by default because, with a single shadow map sample, walls and
 * doors showed diagonal bands (shadow acne) that the Xbox 360 does not have; from a distance they looked
 * blurry. The 9-sample PCF fixed nothing: the grid looked the same with 9 samples as with 1, because it was
 * shadow map acne, which the depth slope bias removes (masseffect_native_shadows_pending_bias). The 9 samples
 * only cost GPU time: together with the blur, GPU time went from 25.1 to 27.3 real ms. So the cheap
 * PCF is enabled by default again.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_pcf_cheap, true, "MASSEFFECT",
                    "Native renderer: a single shadow map sample instead of the "
                    "3x3 half-texel pattern. Smoke (p_000101) takes eleven per pixel on full-screen "
                    "rectangles and is 21 % of the scene. The shadow edge gets slightly less smooth "
                    "(true by default again; the bands on walls were acne and are "
                    "removed by the depth bias). Needs the regenerated shader library");

/*
 * Anisotropic filtering, beyond the Xbox 360 (masseffect_anisotropic_native).
 *
 * The game requests trilinear without anisotropy on all its textures ("C4 filters requested": anisotropy 0,
 * bias 0), and the 435 sampling instructions of its 74 pixel shaders do not change that (all of them "use
 * the constant", read from the original binaries). That is why, on the 360 too, surfaces seen at a grazing
 * angle or from a distance look blurry and gain detail as you get closer: a door seen at an angle,
 * for example. With N > 1, linear samplers with mips get anisotropy N (capped by the device). Single-level
 * ones (resolved targets, shadow map, screen effects) and point samplers stay as they were. It costs GPU time
 * in the scene: measured in "C2: GPU per Swap ... scene". 0 = trilinear, as on the 360.
 */
// 0 by default. At 8x it did not remove the streaks on doors, and the scene went up ~2 ms of GPU
// time together with the 9 shadow samples.
REXCVAR_DEFINE_INT32(masseffect_anisotropic_native, 0, "MASSEFFECT",
                     "Native renderer: anisotropic filtering of world textures "
                     "(linear samplers with mips). 0 = like the Xbox 360 (trilinear, blurry at grazing angles and in the "
                     "distance); 2, 4, 8 or 16 = sharper, at a GPU cost")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * Our own depth bias in the shadow map (acne on walls and doors).
 *
 * Doors and walls showed a fine grid of dots and diagonal streaks that the Xbox 360 does not have.
 * Without the shadow map the grid disappears and the number of PCF samples makes no difference, so it is
 * shadow acne: the surface shadows itself. The game sets no
 * depth bias in that pass (0 "depth bias" lines in every log). Our own is added only to
 * the shadow map draws, and only if the game does not set one. Too high a value detaches the shadows from the
 * objects casting them. 0 = the game's own (no) bias.
 */
REXCVAR_DEFINE_INT32(masseffect_native_shadows_pending_bias, 20, "MASSEFFECT",
                     "Native renderer: slope-scaled depth bias in the shadow map, "
                     "in tenths (20 = 2.0). Removes acne (diagonal dot grid on walls and garage "
                     "doors). Too high detaches shadows from objects. 0 = as before. Can be changed at runtime")
    .range(0, 100);

// Mass Effect requests anisotropy (fetch aniso_filter 3 = 4:1) with a POINT mip filter on its world samplers
// (UE3 AnisotropicPoint). This renderer used to drop the anisotropy and keep the point mip filter: hard
// sharp/blurry bands that crawl with the camera. The SDK (like D3D12) forces linear min/mag/mip whenever
// anisotropy is on. 1 = that (linear mips where the game asks for anisotropy); 2 = also apply the requested
// anisotropy (capped by the device). Costs scene GPU time; measured on the Switch.
REXCVAR_DEFINE_INT32(masseffect_native_filter_aniso, 0, "Mass Effect",
                     "Samplers the game requests with anisotropy: 0 = point mips, no anisotropy (old); 1 = linear "
                     "mips (trilinear, like the SDK); 2 = linear mips + the requested anisotropy")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_skip_prepass, true, "Mass Effect",
                    "Skip the scene depth prepass (EDRAM mode 5 draws at the screen width, not clears): its depth, "
                    "rendered in the 2x view, disagrees with the material pass and caused black shards (B1)");

/*
 * Splitting the frame into two submissions.
 *
 * Measured on the console: there is a single vkQueueSubmit per frame, at the end, from Present. The GPU
 * runs out of work from the end of one frame until the CPU sends the next, and that is ~4 real ms of idle
 * time per frame ("gap between jobs", constant in steady play).
 *
 * Submitting as soon as the shadow pass closes lets the GPU start on the shadows while the CPU records the
 * scene. It does not change a single pixel: it is the same work in two pieces.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_send_after_shadows, false, "MASSEFFECT",
                    "Native renderer: submit work to the GPU as soon as the shadow pass "
                    "closes, instead of all together at the end of the frame. The GPU stops idling "
                    "while the CPU finishes recording");
REXCVAR_DEFINE_INT32(masseffect_native_stats_per_draw_s, 0, "MASSEFFECT",
                     "Native renderer: with N > 0, measures one frame every N "
                     "seconds with one query per draw and distributes fragments per pixel shader");
/*
 * Do not upload the same vertices twice in the same frame.
 *
 * Each draw copies the [vmin..vmax] range of each binding to the upload buffer, byte-swapped. There was no
 * cache: the same piece of geometry was copied again in full every time it was drawn. Measured on PC with
 * masseffect_native_diag_vertices_repeated:
 *     5954.9 MB copied; 2584.2 MB repeated within the same frame and 3329.2 MB equal to an earlier
 *     frame  ->  99.0-99.6 % of the bytes are byte-for-byte repeats, across eight reports.
 * On the console that is 8.4 MB of vertices per frame at 2518 MB/s into uncached memory = 3.3 ms of
 * writing alone, split between the ring and the copy thread.
 *
 * This only does the safe half: repeats within the same frame (41-49 % of the bytes). And it is exact, not a
 * gamble: on the Xbox 360 the GPU reads the vertices when it executes the draw, so the game cannot rewrite a
 * range that is already referenced without first synchronizing with the GPU. The only point where it can is
 * a guest wait (WAIT_REG_MEM), and there everything recorded is forgotten (g_synchronizations_ring).
 *
 * If geometry ever looks stuck or stretched, this is the first thing to disable.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_indices_cache, false, "Mass Effect",
                    "Same-frame index buffer cache (16-bit fast path): a draw that repeats the guest index range "
                    "(address, count, byte order) of an earlier draw in this upload buffer reuses its uploaded copy "
                    "and its min/max instead of converting and copying again (the x5 per-light re-draws). Ranges of "
                    "up to 16 KB are also fingerprinted (transient UI buffers)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_dedupe_max_fingerprint, 0, "Mass Effect",
                     "Same-frame vertex dedupe: ranges larger than this many bytes are matched by address, size and "
                     "byte order only, without the content fingerprint (static meshes; the transient UI/movie buffers "
                     "that need the check are small). 0 = fingerprint every range")
    .range(0, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_compare_fast, false, "Mass Effect",
                    "Per-draw state caches (sampler/fetch constants, pass key, shared block, push constants, viewport, "
                    "pipeline keys) compare their 8-130 byte keys with an inline NEON/word loop instead of a libc memcmp "
                    "call (~15 per draw); the EDRAM trace-tile query returns at once when no tile is traced. Same answers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace masseffect::native {
bool CompareActiveFast() { return REXCVAR_GET(masseffect_native_compare_fast); }  // for masseffect_native_targets.cpp
}  // namespace masseffect::native
REXCVAR_DEFINE_BOOL(masseffect_native_fast_copy, false, "Mass Effect",
                    "Vertex copy into the upload buffer: 64 bytes per iteration with the guest source prefetched ahead "
                    "(the guest data is cold: written frames ago by another core). Same bytes; checked against the plain "
                    "loop for the first masseffect_native_fast_copy_verify copies")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_fast_copy_verify, 256, "Mass Effect",
                     "Self-check of masseffect_native_fast_copy: copies compared with the plain loop (0 = none)")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_fingerprint_mode, 0, "Mass Effect",
                     "Content fingerprint of the sampled vertex/index/texture blocks (SampleFingerprint, index cache): "
                     "0 = XXH3 seeded per block (every seeded call over 240 bytes first regenerates XXH3's 192-byte "
                     "secret), 1 = unseeded XXH3 per block chained with a bijective mix (same 64-bit strength, no secret "
                     "setup), 2 = two-polynomial hardware CRC32 pair (masseffect_crc_fingerprint.h; same 2^-64 argument, needs "
                     "+crc, otherwise mode 1)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_dedupe_sample, true, "Mass Effect",
                    "Vertex dedupe content check hashes sampled blocks (every 8th, SampleFingerprint) instead of the "
                    "whole range. false = full XXH3 of every range, as before");
REXCVAR_DEFINE_BOOL(masseffect_native_dedupe_vertices, true, "MASSEFFECT",
                    "Native renderer: if two draws in the same frame request the same vertex "
                    "range, it is uploaded only once. The C6 report lists hits and MB saved");

/*
 * No vkCmdBindVertexBuffers in single-binding draws.
 *
 * Each draw copies its vertices to a new spot in the upload buffer, so its binding changed almost every
 * time: in play, 0.32-0.46 us per call in 66-98 % of the draws (0.21-0.34 us per draw). In NVK that is
 * three wrapper functions (vk_common_CmdBindVertexBuffers -> 2EXT -> 3KHR) and 5 words with an MME macro per
 * binding.
 *
 * With a single binding, the copy is allocated at a multiple of the stride, the upload buffer stays bound
 * at 0 and the draw is shifted with vertexOffset (indexed) or firstVertex (non-indexed). The GPU reads the
 * same bytes: (offset / stride + i - vmin) * stride = offset + (i - vmin) * stride. The MASSEFFECT shaders do not
 * read SV_VertexID (XenosRecomp only declares it with UNLEASHED_RECOMP and the library is generated with
 * MASSEFFECT_RECOMP), so the base index is not visible anywhere. NVK passes vertexOffset/firstVertex as is to
 * the draw macro on every Draw, with or without this change: recording the draw costs the same.
 *
 * These still bind as usual: draws with two or more bindings, those that reuse (dedupe) a copy that does not
 * start at a multiple of their stride, and the deferred sky, which saves and replays its own binding (and
 * when emitted leaves recorded_bindings_ at 0, so the next draw binds at 0 again).
 *
 * Guard (CheckBaseZero): the first 200,000 draws on this path, then 1 in 4,096, redo the computation
 * backwards in 64 bits and check the limits. On a single difference, that draw binds as usual, MISMATCH
 * is written to the log and the path switches off for the session. What the guard cannot see is the GPU:
 * if anything looked wrong, masseffect_native_vertices_base_zero = false restores the usual path.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_vertices_base_zero, true, "MASSEFFECT",
                    "Native renderer: for draws with a single vertex bind, the upload "
                    "buffer stays bound at 0 and the draw is shifted with vertexOffset/firstVertex, without a "
                    "vkCmdBindVertexBuffers per draw. The GPU reads the same bytes. false = as before");

REXCVAR_DEFINE_BOOL(masseffect_native_ps_alpha_only, true, "MASSEFFECT",
                    "Native renderer: in passes without a color target, compiles the "
                    "pixel shader without the color writes. The image does not change (Vulkan discards them) and the "
                    "driver removes as dead what only fed the color: the alpha test remains");

/*
 * Depth test before shading, where it can be done without changing the image.
 *
 * The problem, measured. The scene costs 13.86 raw ms = 22.55 real ms (in play) and it is pure shading:
 * 6.5 M fragments over 0.92 M pixels, so every screen pixel is shaded 7 times. And our own counter says
 * that 28 % of the color draws prevent early rejection: they have an alpha test or a real kill.
 *
 * Why that costs so much. A pixel shader that can discard forces the hardware to shade first and test
 * depth afterwards (late-Z): otherwise a discarded fragment would already have written its Z. So everything
 * with alpha (smoke, particles, glass, decals) is shaded in full even when it is behind a building. In NVK
 * that is literal: nvk_shader.c only sets SET_API_MANDATED_EARLY_Z when the module declares
 * EarlyFragmentTests, and our modules never declare it.
 *
 * The part that can be fixed, and why it is pixel-identical. The reason for late-Z is the Z write, not the
 * test. If the draw writes neither depth nor stencil, moving the test earlier cannot corrupt anything: there
 * is nothing extra to write. The shader still discards the color the same way. So for every draw with the
 * Z test on, Z write off and stencil off, declaring EarlyFragmentTests is free and exact, and the GPU stops
 * shading what is hidden.
 *
 * Deliberately left out: opaque alpha-tested draws (vegetation) write Z, so they are not touched.
 *
 * The "C6 early Z" report says how many draws are fixed and how many cannot be because they write
 * depth: that second figure is the exact size of what only a depth pre-pass would solve.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_z_early, true, "MASSEFFECT",
                    "Native renderer: for draws that test depth but do NOT "
                    "write it (smoke, particles, glass, decals), declares EarlyFragmentTests in the "
                    "pixel shader so the GPU tests depth BEFORE shading instead of after. "
                    "Without this, any shader with an alpha test or kill shades all its fragments "
                    "even when they end up hidden. The image is identical: there is no Z write to move ahead");
/*
 * The sky is drawn first and shades the whole screen for nothing.
 *
 * Measured, not estimated (FRAGMENT_SHADER_INVOCATIONS counter per draw, 20 frames): the sky dome is a
 * single draw per frame, 480 indices = 160 triangles, and yet it invokes 0.83 M fragments: 90 % of the
 * screen. Its pixel shader has 5 texture samples. Cost ~4.8 real ms of the scene's 19.2 ms, 25 %.
 *
 * Why. The game draws it before the world. At that point the Z-buffer is empty, so nothing can discard it:
 * it shades the whole screen and then the world paints over it.
 *
 * Why deferring it does not change a single pixel. The draw is opaque (ONE/ZERO blending: it does not read
 * the target) and already tests depth with LEQUAL without writing it (RB_DEPTHCONTROL 00700732). So without
 * deferral, wherever the world covers it, the sky's result is overwritten; deferred, in the same place, the
 * depth test discards it before shading. The final color is the same in both cases, pixel for pixel, and
 * depth is not touched before or after because the sky does not write it.
 *
 * Where it is emitted. In the same pass, as soon as a draw arrives that it cannot be moved past (one with
 * blending, which does read the background color, or an opaque one that does not write depth, which the
 * sky would cover because the Z-buffer would not have changed), or when the pass closes, whichever comes
 * first. Every copy or resolve of the target goes through FinishPass (masseffect_native_targets.cpp), so it
 * is emitted there too before anything reads the color.
 *
 * The only thing that could show, and how to recognize it. If the game drew something at exactly the same
 * depth as the dome (far plane z), the LEQUAL test would let the sky through and cover it, where without
 * deferral it would be the other way round. Nothing should be there (the sky is the farthest thing in the
 * scene), but if the sky is ever seen eating very distant geometry, this is why: disable the cvar.
 */
/*
 * A first version broke the image, and this was why.
 *
 * It produced flickering, badly rendered geometry and odd colors in the menus and on characters. The counter
 * said it unambiguously:
 *
 *     "C6 deferred sky: 7.00 detected per frame, 1.00 really deferred"
 *
 * The analysis expected 0.9 draws per frame for that pixel shader. In reality there are seven: the sky dome
 * is not the only user of that shader. Deferring one of the seven breaks the order of the other six, and
 * that is where the artifacts came from.
 *
 * What was missing: the fingerprint identifies the shader, not the draw. Two draws with the same shader and
 * similar state are indistinguishable by this criterion. And the counter that would have exposed it
 * (detected per frame) shipped in the same build as the change instead of before it. The dome draw has to
 * be identified as such (by index count, 480 = 160 triangles, by being the first of the pass, and by
 * checking that it really covers the screen), and verified with the counter before anything is deferred.
 */
/*
 * Enabled again, but it no longer decides alone.
 *
 * With this cvar on, earlier versions broke the image because seven draws share the shader's fingerprint.
 * Now the self-checking guard sits above it: even with the cvar on, nothing is deferred until 90
 * consecutive frames have been seen with a single dome, and as soon as two show up it switches off for the
 * rest of the session. See kSkyFramesTest and CloseFrameOfTheGuardOfTheSky. Enabling it
 * cannot break the image; at worst it does nothing.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_postponed_sky, true, "MASSEFFECT",
                    "Native renderer: defer the sky draw until after the opaque draws "
                    "of the same pass, instead of drawing it first over an empty Z-buffer. It is a single "
                    "opaque draw that tests depth and does not write it, so the image is identical: "
                    "what is overwritten today is discarded by the depth test instead");
/*
 * Draws that paint nothing and are still shaded.
 *
 * Two exact cases, both read from the draw's own registers:
 *
 *  1. Blending is "0 x source + 1 x destination" (ADD) on every written channel: the result is the
 *     destination as is. The draw cannot change a single color pixel.
 *  2. The alpha test function is 0 (NEVER): the recompiled shader always calls clip() (alphaTestValue
 *     returns -1 for case 0), so every fragment dies, and dies before writing depth.
 *
 * In both cases, if the draw also writes neither depth nor stencil, it leaves no trace of any kind and can
 * be skipped entirely. If it writes depth, only its color write is removed (mask 0), which already saves
 * the blending and the write.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_skip_invisibles, true, "MASSEFFECT",
                    "Native renderer: skip draws that cannot change a single pixel "
                    "(blend that copies the destination, or alpha test with the NEVER function). The image is "
                    "identical by definition");
/*
 * Small per-draw savings on the ring thread. Each has its own cvar; the first three have a self-checking
 * guard (the first 200,000 cases, then 1 in 4,096, also go through the usual path and are compared; on a
 * difference, MISMATCH in the log and the saving is switched off).
 */
REXCVAR_DEFINE_BOOL(masseffect_native_framing_cache, true, "MASSEFFECT",
                    "Native renderer: the viewport, ndc and scissor of a draw are reused "
                    "as long as the framing registers (their generation) and the pass do not change. Self-checked. "
                    "false = computed on every draw, as before");
REXCVAR_DEFINE_BOOL(masseffect_native_util_height_memo, true, "MASSEFFECT",
                    "Native renderer: the usable height of the target is stored in the map entry "
                    "of the last pitch, without looking it up on every draw. Self-checked. false = as before");
REXCVAR_DEFINE_BOOL(masseffect_native_key_fast_pass, true, "MASSEFFECT",
                    "Native renderer: if a draw's targets are the same bytes as those "
                    "of the open pass, their XXH3 is not recomputed. Self-checked. false = XXH3 on every draw");
REXCVAR_DEFINE_BOOL(masseffect_native_cvars_per_frame, true, "MASSEFFECT",
                    "Native renderer: masseffect_native_ps_alpha_only and masseffect_native_no_ps_no_color "
                    "are read once per frame and not on every color-less draw. false = as before");
REXCVAR_DEFINE_BOOL(masseffect_native_no_ps_no_color, true, "MASSEFFECT",
                    "Native renderer: for draws that write no color, "
                    "builds the pipeline without a fragment stage if the pixel shader cannot discard pixels "
                    "or write depth. The image does not change and the GPU does no shading (shadow map)");
REXCVAR_DEFINE_BOOL(masseffect_native_fix_tonemap_7e3, false, "Mass Effect",
                    "Legacy diagnostic override of UE3 tone-map c0.x. Off: retain the game's constant; "
                    "HDR resolve now applies RB_COPY_DEST_INFO exponent bias at the actual conversion");
REXCVAR_DEFINE_DOUBLE(masseffect_native_tonemap_7e3_scale, 0.03125, "Mass Effect",
                      "Scale applied to c0.x of the UE3 tone map when the 7e3 correction is active. "
                      "The default is 1/32; allows A/B sweeps without recompiling");
constexpr uint64_t kMeFingerprintTonemap = 0x44EC14CFD16EF8F8ull;
// A review of other Switch projects found that in NVK for Tegra the memory type the SDK picks for uploads
// (without HOST_CACHED) is an NvMap without CPU caching, and wine-nx measured on the console that writing
// there is slow. All vertex copies go to this buffer, so it can be chosen for an A/B test.
REXCVAR_DEFINE_INT32(masseffect_native_upload_memory, 0, "MASSEFFECT",
                     "Native renderer: upload buffer memory. 0 = the one the SDK picks (on the Switch, without "
                     "CPU cache), 1 = with CPU cache (published with vkFlushMappedMemoryRanges before "
                     "submitting), 2 = without CPU cache")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_shared_native_cache, true, "MASSEFFECT",
                    "Native renderer: the per-draw shared constants (488 bytes) "
                    "in a small buffer WITH CPU cache, separate from the upload buffer. Measured in build 155: writing them without "
                    "cache cost 2.0 us per draw and with cache 0.4. Only with UBO constants")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
/*
 * Direct-mapped pipeline cache.
 *
 * PipelineFor costs 0.52-0.76 us per draw in play ("C6 substages"), more than BindVertexBuffers. The
 * one-entry shortcut misses on ~1 in 3 draws (the same ones that then call BindPipeline), and each miss is
 * an XXH3 of 80 bytes, a division by libstdc++'s prime bucket count and 2-3 jumps to nodes scattered across
 * the heap, which with the ring streaming megabytes through the cache almost always miss all the way to
 * memory: ~1.3 us per miss.
 *
 * In front of the map sits a table of 256 contiguous slots (22 KB) indexed by the low bits of the same
 * XXH3, holding the full key: a hit is one memcmp on a single slot. It only stores key -> VkPipeline pairs
 * already in the map, and the map never erases, so it cannot return anything other than what the map
 * would. Guard: the first 200,000 hits, then 1 in 4,096, also look up the map and must get the same
 * VkPipeline; on a difference, MISMATCH in the log and it switches off for the session. false = map only,
 * as before.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_pipelines_direct, true, "MASSEFFECT",
                    "Native renderer: direct-mapped cache of 256 pipelines in front of the "
                    "PipelineFor map. Same result as the map (self-checked). false = map only, as before");
/*
 * Measurement only, it changes no draw. What changes at each vkCmdBindPipeline of the ring: shaders, vertex
 * input, formats, specialization, or only fixed pipeline state (blending, masks, Z, stencil, cull face,
 * topology, bias, primitive restart). The state-only ones are those Vulkan dynamic state would avoid. Two
 * "C6 pipeline changes" lines every 20 s. false = nothing is counted.
 */
// Disabled by default: it was only a measurement and it has already produced its data.
REXCVAR_DEFINE_BOOL(masseffect_native_count_changes_pipeline, false, "MASSEFFECT",
                    "Native renderer: counts what changes on each vkCmdBindPipeline (measurement "
                    "only, changes no draw). C6 pipeline-change lines every 20 s. false = not counted");
/*
 * Dynamic state, phase 0a. A draw's pipeline is looked up with its key in canonical form (Canonicalize):
 * whatever PipelineFor does not read, or reads but Vulkan ignores (the blend equation without blendEnable,
 * the half of the equation whose channels the mask does not write, the Z function without a Z test, stencil
 * operations without stencil, targets not in the pass), is set to a fixed value. The pipeline is the same in
 * everything Vulkan looks at: fewer pipelines and fewer vkCmdBindPipeline calls (the counter's "no effect"
 * category). Guard: the first 200,000 key changes, then 1 in 4,096, compare the fixed state of both keys
 * field by field (FillFixedState, the relevant part of PipelineFor); on a difference, MISMATCH in the
 * log and it switches off for the session. false = the usual key.
 */
// Disabled by default. Measured: PipelineFor got more expensive and no avoidable bind was measured.
REXCVAR_DEFINE_BOOL(masseffect_native_canonical_key, false, "MASSEFFECT",
                    "Native renderer: the pipeline is looked up with the key in canonical form (only "
                    "what Vulkan reads): fewer pipelines and fewer vkCmdBindPipeline. Self-checked. false = the "
                    "usual key");
/*
 * Dynamic state, phase 0b. BeginPass no longer forgets the bound pipeline. Vulkan keeps the binding and
 * the dynamic state across passes of the same command buffer, and NVK only marks state as dirty when a pass
 * begins (nvk_cmd_buffer_dirty_render_pass). The same key carries the same formats and therefore a
 * compatible render pass (compatibility ignores loadOp and storeOp). A new command buffer still starts with
 * nothing bound. It removes the repeated vkCmdBindPipeline of a pass's first draw (the counter's "same key
 * after starting a pass"). false = forgotten at every pass, as before.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_pipeline_between_passes, true, "MASSEFFECT",
                    "Native renderer: the bound pipeline is kept when a pass of the "
                    "same command buffer begins (Vulkan keeps it). false = rebound on every pass, as "
                    "before");
/*
 * Dynamic state, phase 1 (EDS1/EDS2, core in Vulkan 1.3; the console reports API 1.3.354). Cull mode, front
 * face, topology (within its class), Z test, write and function, stencil with its operations, depth bias
 * and primitive restart leave the pipeline: they are set with vkCmdSet* and only when they change. Two draws
 * that only differ in those share a pipeline and need no new vkCmdBindPipeline (3-4.5 us each on the
 * console). Pipelines in this mode carry a flag in the key (fill2) and do not mix with the usual ones;
 * the mode is decided once per command buffer. Guard: the first 200,000 key changes, then 1 in 4,096,
 * compare what was set with what the usual pipeline would carry (FillFixedState); on a difference,
 * MISMATCH in the log and it switches off for the session (pipelines with all state baked in come back,
 * and the draw with the difference already goes out with its own). false = everything in the pipeline, as
 * before.
 */
// Disabled by default. Measured: each BindPipeline went from 2.95 to 4.36 us and PipelineFor from 0.21 to
// 0.47 us per draw, for only 13 % fewer binds: a net loss of ~1 ms of ring time per frame.
REXCVAR_DEFINE_BOOL(masseffect_native_dynamic_state, false, "MASSEFFECT",
                    "Native renderer: cull face, front face, topology, Z, stencil, bias and restart "
                    "with vkCmdSet* (Vulkan 1.3 EDS1/EDS2) instead of in the pipeline: fewer vkCmdBindPipeline. "
                    "Self-checked. false = everything in the pipeline, as before");
/*
 * Dynamic state, phase 2 (VK_EXT_extended_dynamic_state3; NVK exposes it on Maxwell and the SDK enables it). Blending (blendEnable), its equation and each target's color mask
 * are set with vkCmdSet* and stop splitting pipelines: among the first 64 pipelines created, the 12
 * state-only variants differed only in blending. If the SDK or the device does not provide it, it is not
 * used, and one log line says so. Same guard as phase 1. false = blending in the pipeline, as before.
 */
// Disabled by default, for the same measured reason as masseffect_native_dynamic_state (it goes with it).
REXCVAR_DEFINE_BOOL(masseffect_native_dynamic_state3, false, "MASSEFFECT",
                    "Native renderer: blend, equation and color mask with vkCmdSet* "
                    "(VK_EXT_extended_dynamic_state3) instead of in the pipeline. Self-checked. false = in the "
                    "pipeline, as before");
REXCVAR_DEFINE_BOOL(masseffect_native_diag_memory_upload, true, "MASSEFFECT",
                    "Native renderer: when creating the upload buffer, measures once how many MB/s can be written from the "
                    "CPU into each visible memory type (8 MB) and how much publishing them costs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace masseffect::native {

// Incremented by the ring thread when it handles a WAIT_REG_MEM. That is the
// point where vertex deduplication must forget what it recorded.
std::atomic<uint32_t> g_synchronizations_ring{0};
namespace {

/*
 * A pass's category from its render target, in one place.
 *
 * It used to be decided by width: "1600 or more" meant shadows. With the scene at 1920
 * (masseffect_1080p_test) that no longer tells them apart and the scene was counted as shadows, so its
 * category vanished from the report. The shadow map is recognized for what it is: a depth-only target, with
 * no color at all.
 */
inline uint32_t CategoryOfTarget(uint32_t pitch, const uint64_t* keys) {
  const bool color = keys[0] || keys[1] || keys[2] || keys[3];
  if (!color && keys[4] && pitch >= 1600) {
    return kGpuShadows;
  }
  if (pitch >= 1280) {
    return keys[4] ? kGpuScene : kGpuSceneNoDepth;
  }
  if (pitch >= 640) {
    return kGpuMirror;
  }
  if (pitch >= 320) {
    return kGpu320;
  }
  return kGpuSmaller;
}

namespace gr = rex::graphics;
namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;

constexpr uint32_t kRegConstantsVs = 0x4000;
constexpr uint32_t kRegConstantsPs = 0x4400;
constexpr uint32_t kRegFetch = 0x4800;
constexpr uint32_t kRegBooleans = 0x4900;
constexpr uint32_t kRegistersConstants = 0x400;  // 256 constants x 4

constexpr uint32_t kCapacityHeap[4] = {4096, 16, 64, 512};  // 2D, 3D, cube, samplers
// Work slots. The ones actually used are chosen by masseffect_native_slots_work; this is the room reserved
// for them, and it has to match the array in masseffect_native_targets.cpp.
constexpr size_t kSlotsOfWork = 3;
constexpr VkDeviceSize kUploadSize = VkDeviceSize(64) << 20;
// Separate buffer for the shared constants, per slot. ~1,400 draws x 512 bytes = 0.7 MB per frame; if it
// fills up, the work is submitted just as with the upload buffer.
constexpr VkDeviceSize kSharedSize = VkDeviceSize(4) << 20;  // per work slot (there are three)
// The pipeline cache and the prewarm list go in a single file in <NRO folder>/cache/ (nothing loose next to
// the NRO, and a single pipelines .bin). Header "NFPC", version, list bytes and cache bytes (two uint32
// and two uint64), then both parts: the list as is (its "NFPL" header and the records) and the
// vkGetPipelineCacheData data.
constexpr const char* kFolderCache = "cache";
constexpr const char* kFilePipelines = "masseffect_native_pipelines.bin";
constexpr uint32_t kMagicFilePipelines = 0x4350464Eu;  // "NFPC" in little-endian
constexpr uint32_t kVersionFilePipelines = 1;
constexpr size_t kHeaderFilePipelines = 2 * sizeof(uint32_t) + 2 * sizeof(uint64_t);
// The two files used by earlier versions, next to the NRO: if the new one does not exist yet they are read
// once (so the existing cache is not lost) and deleted when the new one is written.
constexpr const char* kFileCacheOld = "masseffect_native_pipelines.bin";
constexpr const char* kFileOldList = "masseffect_native_pipelines_list.bin";
// The pipeline prewarm list. It holds no game data: state keys, formats and fingerprints. A header of four
// uint32 ("NFPL", version, record size and record count) followed by the records (RegisterPipeline).
constexpr uint32_t kMagicPipelinesList = 0x4C50464Eu;  // "NFPL" in little-endian
constexpr uint32_t kVersionPipelinesList = 2;
constexpr size_t kHeaderList = 4 * sizeof(uint32_t);
constexpr size_t kMaxRegistersList = 4096;
// 296 bytes: MASSEFFECT's shader_common.h (g_NdcScale at +280 and g_NdcOffset at +288).
// Shared constants: texture and sampler indices (0-63), booleans, texcoords, half pixel, alpha threshold
// (68) and function (69), NDC (64-73) and g_InputRemap for the 16 locations (74-89).
// 90 words up to g_InputRemap (bytes 296..359) and 32 more for 1/size of the 16 texture slots (bytes
// 360..487), which avoid querying the texture size on every sample.
constexpr uint32_t kSharedWords = 122;
constexpr uint32_t kWordInvSize = 90;
// Constants through a dynamic UBO (masseffect_native_constants_ubo). The bit is SPEC_CONSTANT_CONSTANTS_UBO
// from shader_common.h, and the sizes are the blocks the shaders declare: 256 and 224 float4, and 23 shared
// float4.
constexpr uint32_t kSpecConstantsUbo = uint32_t(1) << 8;
// The shaders take 1/texture size from the shared constants (SPEC_CONSTANT_INV_TEX_SIZE in XenosRecomp)
// and do not query the texture size on every sample.
constexpr uint32_t kSpecInvTexSize = uint32_t(1) << 9;
// The pixel shader is compiled without its color writes (pass without a color target).
constexpr uint32_t kSpecAlphaOnly = uint32_t(1) << 14;
// Cheap PCF. The shaders that sample the shadow map use a 3x3 pattern at half-texel offsets (nine samples
// per pixel; eleven in p_000101, the smoke, which is full-screen quads and 21 % of the scene). With this bit
// the eight outer offsets are set to zero, the nine samples become identical and the compiler merges them
// into one. Shadow edges lose some smoothness. Needs a shader library built with the PCF variant.
constexpr uint32_t kSpecPcfCheap = uint32_t(1) << 15;
// Bits 16-18 = the alpha test function (0-6). See SPEC_CONSTANT_ALPHA_FUNC_SHIFT in shader_common.h: it
// removes the 7-case switch from every pixel shader.
constexpr uint32_t kSpecFunctionAlphaDisplacement = 16;
// Internal bit of the pipeline key that no shader reads (shader_common.h goes up to bit 19). It marks that
// the pixel shader module is the copy with OpExecutionMode EarlyFragmentTests.
constexpr uint32_t kSpecZEarly = uint32_t(1) << 20;
// Renderer-only pipeline/module variant: clamp fragment outputs to the numeric range of Xenos 7e3 RGB
// render targets. The shader sources don't inspect this bit; it only keeps the pipeline key distinct.
constexpr uint32_t kSpecTarget7e3 = uint32_t(1) << 24;
constexpr uint32_t kSpecRectangle = uint32_t(1) << 29;
// The actual depth attachment representation, not merely RB_DEPTH_INFO.
// Kept in canonical / prewarm keys even when fixed state becomes dynamic.
constexpr uint32_t kSpecDepthFloat24Half = uint32_t(1) << 21;
// Renderer-only variant bits; shader_common does not inspect them.
constexpr uint32_t kSpecDepthFloat24Quantize = uint32_t(1) << 30;
constexpr uint32_t kSpecDepthFloat24Round = uint32_t(1) << 31;
// Host raster-grid expansion is independent of shadow-map resolution scaling.
// Keep its safety contract in pipeline/prewarm keys as well as dynamic framing.
constexpr uint32_t kSpecRasterGridX = uint32_t(1) << 22;
// Bits 25-28 carry the exact color-target mask for kSpecTarget7e3. The normal path used to recover it
// from the live Xenos registers, but a prewarm record only has PipelineKey. Keeping the mask in the key
// lets the prewarm thread build the exact same transformed SPIR-V module as the ring.
constexpr uint32_t kSpecMask7e3Displacement = 25;
constexpr uint32_t kSpecMask7e3 = uint32_t(0xF) << kSpecMask7e3Displacement;

/*
 * The render target height comes from the pitch, not from what the game uses.
 *
 * masseffect_native_targets.cpp creates every render target with `height = max(720, pitch)`, because the real
 * height is not known when it is created. Measured result:
 *
 *   scene    pitch 1280 -> image 1280x1280, the game uses 1280x720
 *   cubemap  pitch  320 -> image  320x720,  the game uses  320x256
 *   bloom    pitch  320 -> image  320x720,  the game uses  320x180
 *   bloom    pitch  160 -> image  160x720,  the game uses  160x90
 *
 * And since the passes are opened with loadOp = LOAD and closed with storeOp = STORE, that whole area is
 * read and written in each of the ~27 pass openings per frame: 18.81 Mtexels opened when the game uses
 * ~8.5. That is ~130-165 MB per frame moved for nothing, over a 21.3 GB/s bus that the three cores also
 * share.
 *
 * The Xbox 360 paid none of this: its framebuffer lived in 10 MB of on-chip EDRAM at 256 GB/s, and the
 * resolve was a hardware operation. We move it back and forth.
 *
 * It is fixed through the renderArea, not the image size. Vulkan only loads and stores the renderArea;
 * everything outside is preserved. So the height does not have to be guessed when the image is created:
 * opening the pass over the rectangle the game really uses is enough.
 *
 * How that height is known without guessing: from the draws' scissor. The maximum seen per pitch is kept,
 * only grows, and is rounded up to a multiple of 64. Until there is data the whole pass is opened, so the
 * first frame never clips too much.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_pass_area_util, true, "MASSEFFECT",
                    "Native renderer: open each pass over the rectangle the game really uses "
                    "instead of over the whole image. The target height is deduced from the pitch, "
                    "so the scene opens 1280x1280 to draw 1280x720 and the cube 320x720 to draw "
                    "320x256. The image is identical: Vulkan only loads and stores the renderArea");
/*
 * The pixel shader of the sky dome (masseffect_native_postponed_sky).
 *
 * The fingerprint is the XXH3 of the shader's original container, not of the translated SPIR-V: it
 * identifies the game's shader and does not change when the library is regenerated (checked: the same
 * 28AA3CDAC6C19705 in masseffect_validated_predicates and in masseffect_fusion3, with SPIR-V of 1,859 and 3,306
 * words). It is the same mechanism as the other fingerprint checks.
 *
 * What it is, beyond doubt: its constant table declares CloudIntensity, SkyAlphaTag and Brightness, and
 * four samplers (DIFFUSEMAP, MISCMAP1, MISCMAP2, MISCMAP3) with 5 samples. It is the only shader in the
 * library (152 containers) that names the sky: it cannot be confused with any other. In the log it shows
 * up as PS n29.
 */
constexpr uint64_t kSkyFingerprint = 0x28AA3CDAC6C19705ull;
// The sky dome is 480 indices = 160 triangles, measured in the log (the C6 diag line of the draw with
// PS n29). That is the mark that tells the dome draw apart from the other six that share its pixel
// shader, which were the ones that broke the image in an earlier version.
constexpr uint32_t kIndicesDomeSky = 480;
/*
 * How long the sky guard's test lasts, counting only the frames in which the dome appears (menus and
 * loading screens do not count, see CloseFrameOfTheGuardOfTheSky). 90 frames are about 3 seconds
 * of play: enough not to decide on four samples, and short enough for the saving to start almost as
 * the scene begins. All 90 must have exactly one dome: as soon as two appear in the same frame (the
 * earlier failure) it switches off for good.
 */
constexpr uint32_t kSkyFramesTest = 90;
constexpr uint32_t kSkyFramesWithOne = 90;
constexpr VkDeviceSize kUboBytesVs = 256 * 16;
constexpr VkDeviceSize kUboBytesPs = 224 * 16;
constexpr VkDeviceSize kUboBytesShared = 31 * 16;
constexpr uint32_t kRemapIdentity = 0xFFF;
constexpr uint32_t kMaxVerticesPerDraw = uint32_t(1) << 20;
constexpr uint32_t kPhysicalMemory = 0x20000000;
// Times a statement in the timed draws (C6 substages).
#define MASSEFFECT_SUB(k, ...) \
  do { \
    if (time_) { \
      const auto t0_sub_ = std::chrono::steady_clock::now(); \
      __VA_ARGS__; \
      sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>( \
                                 std::chrono::steady_clock::now() - t0_sub_) \
                                 .count()); \
      ++sub_n_[k]; \
    } else { \
      __VA_ARGS__; \
    } \
  } while (0)

/*
 * From 8 to 64. Each timed draw reads the clock about 20 times (Stage, MASSEFFECT_SUB, samplers and
 * vertices). At 1 in 8 that averaged 0.15-0.4 us on every draw of the ring, and a busy scene has
 * 2,500-4,000 per frame. "C6 stages", "C6 substages" and "ms copying vertices" divide (or rescale) by
 * the draws that actually carried a stopwatch, so their scale does not change: they just have 8 times
 * fewer samples (~600-1,400 per second in play).
 */
constexpr uint32_t kStopwatchEach = 64;  // draws per stage-timed draw (power of 2)
constexpr size_t kCopiesPerWarning = 64;   // copies queued between wake-ups of the copy thread (power of 2)
// Copies taken in one batch (masseffect_native_uploads_help). In play they are ~2 KB each: a batch is
// ~30 KB, about 20 us. That is how long the ring may have to wait for the thread, already with its
// priority lent.
constexpr size_t kCopiesPerChunk = 16;
// Waits with pending copies in the observing phase (the ring waits as before and checks the marks)
// before it starts helping. In the menus that is a few seconds.
constexpr uint64_t kWaitsCopiesLooking = 512;

// One guest vertex binding into the upload buffer, as host words with the fetch constant's byte order.
// Done by the ring thread or by the copy thread (masseffect_native_uploads_thread).
struct WorkCopy {
  const uint8_t* source = nullptr;
  uint8_t* target = nullptr;
  uint32_t words = 0;
  xenos::Endian order = xenos::Endian::kNone;
};

void CopyVerticesBase(const WorkCopy& t) {
  // The fields go into local variables: the destination is a byte pointer and, as far as the compiler
  // knows, writing through it could change the structure itself. With t.* inside the loop it was not
  // vectorized (935 ms per 5.3 GB of vertices in play, against 490 ms with the loop inside Draw).
  const uint8_t* const source = t.source;
  uint8_t* const target = t.target;
  const uint32_t words = t.words;
  const xenos::Endian order = t.order;
  if (order == xenos::Endian::k8in32) {
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      v = std::byteswap(v);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  } else {
    for (uint32_t i = 0; i < words; ++i) {
      uint32_t v;
      std::memcpy(&v, source + size_t(i) * 4, 4);
      v = xenos::GpuSwap(v, order);
      std::memcpy(target + size_t(i) * 4, &v, 4);
    }
  }
}

// masseffect_native_fast_copy: the same swap and copy, 64 bytes per iteration (four 16-byte loads, then four stores
// that the compiler pairs into stp q) with the source prefetched 640 bytes ahead. The guest's vertex data was written
// frames ago by another core, so its lines are cold; the plain loop waits for each one. The first copies are compared
// with the plain loop (MISMATCH, plain loop from then on and for that copy).
template <int kOrder>
inline uint8x16_t PermuteBytes(uint8x16_t v) {
  if constexpr (kOrder == 1) return vrev16q_u8(v);                                                       // 8in16
  else if constexpr (kOrder == 2) return vrev32q_u8(v);                                                  // 8in32
  else if constexpr (kOrder == 3) return vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));     // 16in32
  else return v;
}
template <int kOrder>
void CopyBlocks64(const uint8_t* source, uint8_t* target, size_t blocks) {
  for (size_t b = 0; b < blocks; ++b) {
    __builtin_prefetch(source + 640);
    const uint8x16_t a = vld1q_u8(source), c = vld1q_u8(source + 16), d = vld1q_u8(source + 32), e = vld1q_u8(source + 48);
    vst1q_u8(target, PermuteBytes<kOrder>(a));
    vst1q_u8(target + 16, PermuteBytes<kOrder>(c));
    vst1q_u8(target + 32, PermuteBytes<kOrder>(d));
    vst1q_u8(target + 48, PermuteBytes<kOrder>(e));
    source += 64;
    target += 64;
  }
}

std::atomic<int32_t> g_fast_copy_verify{-1};  // -1 = not initialised, then the uses still to check
std::atomic<bool> g_fast_off_copy{false};

void CopyVertices(const WorkCopy& t) {
  static const bool fast = REXCVAR_GET(masseffect_native_fast_copy);
  if (fast && t.words >= 32 && !g_fast_off_copy.load(std::memory_order_relaxed)) {
    const size_t blocks = size_t(t.words) / 16;
    switch (t.order) {
      case xenos::Endian::k8in16: CopyBlocks64<1>(t.source, t.target, blocks); break;
      case xenos::Endian::k8in32: CopyBlocks64<2>(t.source, t.target, blocks); break;
      case xenos::Endian::k16in32: CopyBlocks64<3>(t.source, t.target, blocks); break;
      default: CopyBlocks64<0>(t.source, t.target, blocks); break;
    }
    const uint32_t done = uint32_t(blocks * 16);
    if (done < t.words) {
      CopyVerticesBase({t.source + size_t(done) * 4, t.target + size_t(done) * 4, t.words - done, t.order});
    }
    int32_t remain = g_fast_copy_verify.load(std::memory_order_relaxed);
    if (remain < 0) {
      int32_t initial = REXCVAR_GET(masseffect_native_fast_copy_verify);
      g_fast_copy_verify.compare_exchange_strong(remain, initial);
      remain = g_fast_copy_verify.load(std::memory_order_relaxed);
    }
    if (remain > 0 && g_fast_copy_verify.fetch_sub(1, std::memory_order_relaxed) > 0) {
      std::vector<uint8_t> expected(size_t(t.words) * 4);
      CopyVerticesBase({t.source, expected.data(), t.words, t.order});
      if (std::memcmp(expected.data(), t.target, expected.size()) != 0) {
        g_fast_off_copy.store(true, std::memory_order_relaxed);
        REXLOG_ERROR("[native] MISMATCH: the fast vertex copy ({} words, order {}) does not give the same bytes as "
                     "the plain loop; using the plain loop from now on", t.words, uint32_t(t.order));
        std::memcpy(t.target, expected.data(), expected.size());
      }
    }
    return;
  }
  CopyVerticesBase(t);
}

// Allocator that leaves whatever resize adds uninitialized: indices_, converted_ and indices16_ are
// fully rewritten right after, and std::vector's zero fill was 1.4 % of the ring thread on PC.
template <class T>
struct NoInitialize : std::allocator<T> {
  using value_type = T;
  NoInitialize() = default;
  template <class U>
  NoInitialize(const NoInitialize<U>&) noexcept {}
  template <class U>
  struct rebind {
    using other = NoInitialize<U>;
  };
  template <class U>
  void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>) {
    ::new (static_cast<void*>(p)) U;
  }
  template <class U, class... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};

using FnBufferAddress = VkDeviceAddress(VKAPI_PTR*)(VkDevice, const VkBufferDeviceAddressInfo*);

// Inline equality of two small blocks (the per-draw state caches compare 8-130 bytes several times per draw; a libc memcmp
// call each cost ~0.6 ms/frame on the Switch ring thread). Same answer as std::memcmp(a, b, n) == 0.
inline bool EqualInLine(const void* a, const void* b, size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(a);
  const uint8_t* q = static_cast<const uint8_t*>(b);
  uint64_t rest = 0;
  uint8x16_t accumulated = vdupq_n_u8(0);
  while (n >= 16) {
    accumulated = vorrq_u8(accumulated, veorq_u8(vld1q_u8(p), vld1q_u8(q)));
    p += 16;
    q += 16;
    n -= 16;
  }
  rest = vmaxvq_u8(accumulated);
  while (n >= 8) {
    uint64_t x, y;
    std::memcpy(&x, p, 8);
    std::memcpy(&y, q, 8);
    rest |= x ^ y;
    p += 8;
    q += 8;
    n -= 8;
  }
  if (n >= 4) {
    uint32_t x, y;
    std::memcpy(&x, p, 4);
    std::memcpy(&y, q, 4);
    rest |= x ^ y;
    p += 4;
    q += 4;
    n -= 4;
  }
  while (n) {
    rest |= uint8_t(*p ^ *q);
    ++p;
    ++q;
    --n;
  }
  return rest == 0;
}

// masseffect_native_compare_fast selects the inline version; off = the libc memcmp as before.
inline bool Equal(const void* a, const void* b, size_t n) {
  static const bool in_line = REXCVAR_GET(masseffect_native_compare_fast);
  return in_line ? EqualInLine(a, b, n) : std::memcmp(a, b, n) == 0;
}

float Float(uint32_t value) {
  return std::bit_cast<float>(value);
}

// Arithmetic of the Xenos mip level layout (pipeline/texture/util.cpp: GetPackedMipLevel,
// GetPackedMipOffset and GetGuestTextureLayout).
uint32_t Log2Ceiling(uint32_t v) {
  return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1));
}

uint32_t Log2Floor(uint32_t v) {
  return v ? 31 - uint32_t(std::countl_zero(v)) : 0;
}

/*
 * The sample used to recheck a stable texture (masseffect_native_fingerprints_sampling). From a region of guest
 * memory, its first 4 KB block, its last one and one in every kSampleEach are read, counted from the
 * start of the region. The base and the mips of a texture start at 4 KB-aligned addresses, so each block
 * is a whole page. That reads ~13-19 % of the bytes of textures of 64 KB or more; for textures of few
 * blocks the sample is almost everything, and PrepareTexture does not use it if it exceeds half. Tested
 * on PC: BytesSample matches what is read for every size from 1 byte to 300 KB, and a one-byte change is
 * seen if and only if it falls in a sampled block.
 */
constexpr uint64_t kBlockSample = 4096;
constexpr uint64_t kSampleEach = 8;

// Bytes SampleFingerprint reads in a region of that size.
inline uint64_t BytesSample(uint64_t bytes) {
  if (!bytes) {
    return 0;
  }
  const uint64_t last = (bytes - 1) / kBlockSample;
  return (last + kSampleEach - 1) / kSampleEach * kBlockSample + (bytes - last * kBlockSample);
}

// masseffect_native_fingerprint_mode: how a block's bytes are folded into the running fingerprint.
inline int ModeFingerprint() {
  static const int mode = [] {
    int m = REXCVAR_GET(masseffect_native_fingerprint_mode);
    if (m == 2 && !crc_fingerprint::kAvailable) m = 1;
    return m;
  }();
  return mode;
}

// Mode 1: chains an unseeded block hash into the previous fingerprint. Bijective in the block hash for a fixed previous
// fingerprint and in the previous fingerprint for a fixed block hash, so two inputs differing in one block always get
// different fingerprints unless the 64-bit block hashes collide; several differing blocks collide with probability 2^-64.
inline uint64_t ChainFingerprint(uint64_t previous, uint64_t block) {
  uint64_t m = previous * 0x9E3779B97F4A7C15ull;
  m ^= m >> 32;
  m *= 0xC2B2AE3D27D4EB4Full;
  m ^= m >> 29;
  return m + block;
}

inline uint64_t BlockFingerprint(const uint8_t* data, size_t bytes, uint64_t previous) {
  switch (ModeFingerprint()) {
#if MASSEFFECT_CRC_FINGERPRINT
    case 2:
      return crc_fingerprint::Fingerprint64(data, bytes, previous);
#endif
    case 1:
      return ChainFingerprint(previous, XXH3_64bits(data, bytes));
    default:
      return XXH3_64bits_withSeed(data, bytes, previous);
  }
}

// Unseeded fingerprint of a whole range (the 16-bit index cache).
inline uint64_t RangeFingerprint(const uint8_t* data, size_t bytes) {
#if MASSEFFECT_CRC_FINGERPRINT
  if (ModeFingerprint() == 2) return crc_fingerprint::Fingerprint64(data, bytes, 0);
#endif
  return XXH3_64bits(data, bytes);
}

// Chained fingerprint of blocks 0, 8, 16... below the last one, and of the last one (which may be partial).
inline uint64_t SampleFingerprint(const uint8_t* data, uint64_t bytes, uint64_t seed) {
  if (!bytes) {
    return seed;
  }
  const uint64_t last = (bytes - 1) / kBlockSample;
  uint64_t fingerprint = seed;
  for (uint64_t b = 0; b < last; b += kSampleEach) {
    fingerprint = BlockFingerprint(data + b * kBlockSample, size_t(kBlockSample), fingerprint);
  }
  return BlockFingerprint(data + last * kBlockSample, size_t(bytes - last * kBlockSample), fingerprint);
}

// First level of the packed tail: once the short side is 16 texels or less.
uint32_t PackedLevel(uint32_t width, uint32_t height) {
  const uint32_t l = Log2Ceiling(std::min(width, height));
  return l > 4 ? l - 4 : 0;
}

// Blocks from the start of the packed tail to a level of a 2D texture; 0 if the level is not packed.
void PackedDisplacement(uint32_t width, uint32_t height, uint32_t block, uint32_t level, uint32_t& x,
                               uint32_t& y) {
  const uint32_t l2_width = Log2Ceiling(width);
  const uint32_t l2_height = Log2Ceiling(height);
  const uint32_t l2 = std::min(l2_width, l2_height);
  x = 0;
  y = 0;
  if (l2 > 4 + level) {
    return;
  }
  const uint32_t base = l2 > 4 ? l2 - 4 : 0;
  const uint32_t m = level - base;
  if (m < 3) {
    if (l2_width > l2_height) {
      y = 16u >> m;  // wider than tall: levels are stacked vertically
    } else {
      x = 16u >> m;
    }
  } else if (l2_width > l2_height) {
    x = (1u << (l2_width - base)) >> (m - 2);
  } else {
    y = (1u << (l2_height - base)) >> (m - 2);
  }
  x /= block;
  y /= block;
}

// Address of a block in a texture tiled in 32x32 blocks, copied from GetTiledOffset2D
// (graphics/pipeline/texture/util.cpp). pitch in blocks.
int32_t TileDisplacement2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// Fast untiling. During stutters the ring spent ~30 ms preparing 5 MB of new textures (about 6 ms
// per MB): ReadLevel called TileDisplacement2D and a variable-size memcpy per block. Here, with the
// block size fixed at compile time:
//   - what depends on the row (y) is computed once per row;
//   - within a 16-byte group of the tiling the blocks are contiguous in the source (only the 4 low bits
//     of micro change), so 16 bytes are copied at once (8 for textures with 1 byte per block).
// It gives exactly the same addresses as TileDisplacement2D (checked block by block on PC).
template <uint32_t kLog2>
void UntileLevel(const uint8_t* source, uint32_t pitch, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by,
                        uint32_t bx_host, uint8_t* target) {
  constexpr uint32_t kBytes = 1u << kLog2;
  constexpr uint32_t kGroup = (16u >> kLog2) < 8u ? (16u >> kLog2) : 8u;  // blocks contiguous in the source
  const int32_t macros_row = int32_t(((pitch + 31) & ~uint32_t(31)) >> 5);
  for (uint32_t row = 0; row < by; ++row) {
    const int32_t y = int32_t(oy + row);
    const int32_t macro_y = (y >> 5) * macros_row;
    const int32_t micro_y = (y & 0xE) << 2;
    const int32_t y1 = (y & 1) << 4;
    const int32_t y16 = (y & 16) << 7;
    const int32_t y8 = (y & 8) >> 2;
    uint8_t* output = target + size_t(row) * bx_host * kBytes;
    uint32_t column = 0;
    while (column < bx) {
      const int32_t x = int32_t(ox + column);
      const int32_t macro = ((x >> 5) + macro_y) << (kLog2 + 7);
      const int32_t micro = ((x & 7) + micro_y) << kLog2;
      const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + y1;
      const int32_t displacement = ((offset & ~0x1FF) << 3) + y16 + ((offset & 0x1C0) << 2) +
                                     (((y8 + (x >> 3)) & 3) << 6) + (offset & 0x3F);
      if (kGroup > 1 && (uint32_t(x) % kGroup) == 0 && column + kGroup <= bx) {
        std::memcpy(output + size_t(column) * kBytes, source + displacement, kGroup * kBytes);
        column += kGroup;
      } else {
        std::memcpy(output + size_t(column) * kBytes, source + displacement, kBytes);
        ++column;
      }
    }
  }
}

// Byte swap of a whole texture in one go (instead of GpuSwap word by word, with the switch on the order
// inside the loop). Same result as GpuSwap: for 16-bit units only k8in16 changes anything; for 32-bit
// units, k8in16, k8in32 and k16in32. It only touches complete units, like the plain loop.
inline void ChangeOrderBytes(uint8_t* data, size_t bytes, uint32_t unit, uint32_t order) {
  constexpr uint32_t k8in16 = 1, k8in32 = 2, k16in32 = 3;  // xenos::Endian
  size_t n = unit == 2 ? bytes & ~size_t(1) : bytes & ~size_t(3);
  if ((unit == 2 && order != k8in16) || (unit != 2 && unit != 4) || order == 0) {
    return;
  }
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    const uint8x16_t v = vld1q_u8(data + i);
    uint8x16_t r;
    if (unit == 2 || order == k8in16) {
      r = vrev16q_u8(v);
    } else if (order == k8in32) {
      r = vrev32q_u8(v);
    } else {
      r = vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
    }
    vst1q_u8(data + i, r);
  }
  if (unit == 2 || order == k8in16) {
    for (; i + 2 <= n; i += 2) {
      std::swap(data[i], data[i + 1]);
    }
  } else if (order == k8in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
    }
  } else if (order == k16in32) {
    for (; i + 4 <= n; i += 4) {
      std::swap(data[i], data[i + 2]);
      std::swap(data[i + 1], data[i + 3]);
    }
  }
}

// The same for a 3D texture tiled in 32x32x4 blocks, copied from GetTiledOffset3D
// pitch and height in blocks.
int32_t TileDisplacement3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                                uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  height = (height + 31) & ~uint32_t(31);
  const int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_outer) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t outer = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = outer + ((((x >> 3) + (outer << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// USAGE_LOCATIONS from XenosRecomp (shader_recompiler.cpp).
int32_t LocationOfUsage(uint8_t usage, uint8_t index) {
  switch (usage) {
    case 0:  // position
      return index == 0 ? 0 : (index == 1 ? 15 : -1);
    case 3:  // normal
      return index == 0 ? 1 : -1;
    case 6:  // tangent
      return index == 0 ? 2 : -1;
    case 7:  // binormal
      return index == 0 ? 3 : -1;
    case 5:  // texcoord
      return index < 4 ? 4 + index : (index < 8 ? 12 + (index - 4) : -1);
    case 10:  // color
      return index == 0 ? 8 : (index == 1 ? 11 : -1);
    case 2:  // blend indices
      return index == 0 ? 9 : -1;
    case 1:  // blend weights
      return index == 0 ? 10 : -1;
    default:
      return -1;
  }
}

// USAGE_TYPES: these inputs are uint4 in the translated shaders.
bool WholeEntry(uint8_t usage) {
  // Only BLENDINDICES. Since the masseffect_validated_normal library, normals, tangents and binormals are
  // float4: MASSEFFECT stores them as 16-bit integers (format 26) or as floats, and with uint4 the shader's
  // asfloat gave degenerate directions (seen on reflective surfaces).
  return usage == 2;
}

// g_InputRemap code: the SPIR-V writes r[i] = input[orig[i]] and the fetch patched by D3D writes
// r[i] = data[patched[i]] (or 0 / 1). For each written component, the host input at orig[i] has to
// come from patched[i]. 7 = the same component.
uint32_t RemapCode(uint32_t original, uint32_t patched) {
  uint32_t code = kRemapIdentity;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t o = (original >> (i * 3)) & 0x7;
    const uint32_t d = (patched >> (i * 3)) & 0x7;
    if (o <= 3 && d != 7) {
      code = (code & ~(uint32_t(0x7) << (o * 3))) | (d << (o * 3));
    }
  }
  return code;
}

// Components a host vertex format delivers; Vulkan fills the missing ones with (0, 0, 1).
uint32_t ComponentsVertexFormat(VkFormat f) {
  switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_UINT:
      return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_SSCALED: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_USCALED: case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_UINT:
      return 2;
    case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT:
      return 3;
    default:
      return 4;  // also anything unknown: no normalization of missing components
  }
}

// The same remap code with every "no change" slot written as 7, so the shader's 0xFFF fast path is taken
// (remapInput's per-vertex select chain was ~70 % of the ALU of typical vertex shaders): a slot reading its
// own component, or the constant a missing component already has (0 for y/z, 1 for w).
uint32_t NormalizeRemap(uint32_t code, uint32_t components) {
  for (uint32_t o = 0; o < 4; ++o) {
    const uint32_t d = (code >> (o * 3)) & 0x7;
    const bool equal = d == o || (o >= components && ((o == 3 && d == 5) || (o != 3 && d == 4)));
    if (equal) code |= uint32_t(0x7) << (o * 3);
  }
  return code;
}

VkFormat AttributeFormat(uint32_t format, bool whole_entry, bool with_sign, bool whole,
                         bool red_blue, bool& r11g11b10) {
  r11g11b10 = false;
  const auto choose = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled,
                          VkFormat uint_, VkFormat sint) {
    if (whole_entry) {
      return with_sign ? sint : uint_;
    }
    if (whole) {
      return with_sign ? sscaled : uscaled;
    }
    return with_sign ? snorm : unorm;
  };
  switch (format) {
    case 6:  // k_8_8_8_8
      return red_blue ? choose(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM,
                                VK_FORMAT_B8G8R8A8_USCALED, VK_FORMAT_B8G8R8A8_SSCALED,
                                VK_FORMAT_B8G8R8A8_UINT, VK_FORMAT_B8G8R8A8_SINT)
                       : choose(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM,
                                VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED,
                                VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT);
    case 7:  // k_2_10_10_10
      if (red_blue) break;
      return choose(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                    VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                    VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32);
    case 16:  // k_10_11_11: packed normal decoded by the shader itself
      if (!whole_entry || red_blue) break;
      r11g11b10 = true;
      return VK_FORMAT_R32_UINT;
    case 25:  // k_16_16
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                    VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT);
    case 26:  // k_16_16_16_16
      if (red_blue) break;
      return choose(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                    VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED,
                    VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT);
    case 31:  // k_16_16_FLOAT
      return whole_entry || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16_SFLOAT;
    case 32:  // k_16_16_16_16_FLOAT
      return whole_entry || red_blue ? VK_FORMAT_UNDEFINED : VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33:  // k_32
      return whole_entry && !red_blue ? (with_sign ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT)
                                         : VK_FORMAT_UNDEFINED;
    case 34:  // k_32_32
      return whole_entry && !red_blue
                 ? (with_sign ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT)
                 : VK_FORMAT_UNDEFINED;
    case 35:  // k_32_32_32_32
      return whole_entry && !red_blue
                 ? (with_sign ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                 : VK_FORMAT_UNDEFINED;
    // Floats: a uint4 input reinterprets the bits (asfloat in the shader).
    case 36:  // k_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (whole_entry ? VK_FORMAT_R32_UINT : VK_FORMAT_R32_SFLOAT);
    case 37:  // k_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (whole_entry ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32_SFLOAT);
    case 57:  // k_32_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (whole_entry ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT);
    case 38:  // k_32_32_32_32_FLOAT
      return red_blue ? VK_FORMAT_UNDEFINED
                       : (whole_entry ? VK_FORMAT_R32G32B32A32_UINT
                                         : VK_FORMAT_R32G32B32A32_SFLOAT);
    default:
      break;
  }
  return VK_FORMAT_UNDEFINED;
}

constexpr uint16_t kSwizzleRRRR = 0;
constexpr uint16_t kSwizzleRGGG = (1 << 3) | (1 << 6) | (1 << 9);
constexpr uint16_t kSwizzleRGBA = (1 << 3) | (2 << 6) | (3 << 9);
constexpr uint16_t kSwizzleBGRA = 2 | (1 << 3) | (0 << 6) | (3 << 9);

struct TextureFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint8_t block = 1;       // texels per block side
  uint8_t bytes = 1;        // bytes per block
  uint8_t unit_order = 0;  // 0 = no byte swap, else 2 or 4 bytes
  uint16_t swizzle_host = kSwizzleRGBA;
};

// Same host formats as the emulation.
bool TextureFormatFor(uint32_t format, TextureFormat& f) {
  switch (format) {
    case 2:  // k_8
    case 8:  // k_8_A
      f = {VK_FORMAT_R8_UNORM, 1, 1, 0, kSwizzleRRRR};
      return true;
    case 10:  // k_8_8
      f = {VK_FORMAT_R8G8_UNORM, 1, 2, 2, kSwizzleRGGG};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
      f = {VK_FORMAT_R8G8B8A8_UNORM, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
      f = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 4, 4, kSwizzleRGBA};
      return true;
    case 18:  // k_DXT1
    case 51:
      f = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8, 2, kSwizzleRGBA};
      return true;
    case 19:  // k_DXT2_3
    case 52:
      f = {VK_FORMAT_BC2_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 20:  // k_DXT4_5
    case 53:
      f = {VK_FORMAT_BC3_UNORM_BLOCK, 4, 16, 2, kSwizzleRGBA};
      return true;
    case 49:  // k_DXN
      f = {VK_FORMAT_BC5_UNORM_BLOCK, 4, 16, 2, kSwizzleRGGG};
      return true;
    case 59:  // k_DXT5A
      f = {VK_FORMAT_BC4_UNORM_BLOCK, 4, 8, 2, kSwizzleRRRR};
      return true;
    case 24:  // k_16
      f = {VK_FORMAT_R16_UNORM, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 25:  // k_16_16
      f = {VK_FORMAT_R16G16_UNORM, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 26:  // k_16_16_16_16
      f = {VK_FORMAT_R16G16B16A16_UNORM, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 30:  // k_16_FLOAT
      f = {VK_FORMAT_R16_SFLOAT, 1, 2, 2, kSwizzleRRRR};
      return true;
    case 31:  // k_16_16_FLOAT
      f = {VK_FORMAT_R16G16_SFLOAT, 1, 4, 2, kSwizzleRGGG};
      return true;
    case 32:  // k_16_16_16_16_FLOAT
      f = {VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8, 2, kSwizzleRGBA};
      return true;
    case 36:  // k_32_FLOAT
      f = {VK_FORMAT_R32_SFLOAT, 1, 4, 4, kSwizzleRRRR};
      return true;
    case 37:  // k_32_32_FLOAT
      f = {VK_FORMAT_R32G32_SFLOAT, 1, 8, 4, kSwizzleRGGG};
      return true;
    case 38:  // k_32_32_32_32_FLOAT
      f = {VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16, 4, kSwizzleRGBA};
      return true;
    default:
      return false;
  }
}

bool IsDepth(VkFormat format) {
  return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

// GuestToHostSwizzle: the fetch constant's swizzle, mapped through the channels the host format has.
VkComponentMapping MappingComponents(uint32_t swizzle_guest, uint16_t swizzle_host) {
  VkComponentMapping mapping{};
  VkComponentSwizzle* output[4] = {&mapping.r, &mapping.g, &mapping.b, &mapping.a};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t value = (swizzle_guest >> (i * 3)) & 0x7;
    if (value <= 3) {
      const uint32_t host = (swizzle_host >> (value * 3)) & 0x7;
      *output[i] = VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + host);
    } else if (value == 4) {
      *output[i] = VK_COMPONENT_SWIZZLE_ZERO;
    } else {
      *output[i] = VK_COMPONENT_SWIZZLE_ONE;
    }
  }
  return mapping;
}

// Four post-swizzle TextureSign values, two bits per sampled component. This is the same mapping as
// pipeline::texture::SwizzleSigns in the stock renderer. The native shader ABI packs it into bits 24-31
// of the descriptor index; the descriptor heap itself uses only the low 24 bits.
uint8_t RemappedSigns(const uint32_t* fetch) {
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  uint8_t signs = 0;
  bool signed_any = false, any_no_signed = false;
  uint8_t constant_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t component = (swizzle >> (i * 3)) & 0x7;
    if (component & 0x4) {
      constant_mask |= uint8_t(1) << (i * 2);
      continue;
    }
    const uint32_t sign = (fetch[0] >> (2 + component * 2)) & 0x3;
    signs |= uint8_t(sign << (i * 2));
    signed_any |= sign == uint32_t(xenos::TextureSign::kSigned);
    any_no_signed |= sign != uint32_t(xenos::TextureSign::kSigned);
  }
  uint32_t sign_constants = uint32_t(xenos::TextureSign::kUnsigned);
  if ((constant_mask == 0x55 && ((fetch[0] >> 2) & 0xFF) == 0x55) ||
      (constant_mask != 0x55 && signed_any && !any_no_signed)) {
    sign_constants = uint32_t(xenos::TextureSign::kSigned);
  }
  signs |= uint8_t(sign_constants) * constant_mask;
  return signs;
}

struct AttributeVertices {
  uint32_t location;
  uint32_t binding;
  VkFormat format;
  uint32_t offset;
};

struct BindingVertices {
  uint32_t slot;   // fetch constant de vertices (0-95)
  uint32_t stride;  // bytes
};

struct VerticesEntry {
  std::vector<AttributeVertices> attributes;
  std::vector<BindingVertices> bindings;
  std::array<uint32_t, 16> remaps{};  // g_InputRemap by location
  uint32_t specialization = 0;
  uint64_t fingerprint = 0;
};

// Copy of the SPIR-V without the writes to the color targets (output variables with Location 0..3).
// Returns empty if the module cannot be walked or there was nothing to remove.
std::vector<uint32_t> PruneWritesOfColor(const std::vector<uint32_t>& spirv, uint32_t& removed) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpDecorate = 71, kOpVariable = 59, kOpStore = 62;
  constexpr uint32_t kOpAccessChain = 65, kOpInBoundsAccessChain = 66;
  constexpr uint32_t kDecorationBuiltIn = 11, kDecorationLocation = 30, kDecorationIndex = 29;
  constexpr uint32_t kStoreOutput = 3;
  removed = 0;
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    return {};
  }
  std::unordered_set<uint32_t> with_location, discarded;
  // 1) decorations: Location 0..3 marks a color target; BuiltIn or Index != 0 rule it out (gl_FragDepth
  // and the second blend source are not touched).
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      return {};
    }
    if (code == kOpDecorate && words >= 3) {
      const uint32_t goal = spirv[i + 1];
      const uint32_t decoration = spirv[i + 2];
      if (decoration == kDecorationLocation && words >= 4 && spirv[i + 3] <= 3) {
        with_location.insert(goal);
      } else if (decoration == kDecorationBuiltIn ||
                 (decoration == kDecorationIndex && words >= 4 && spirv[i + 3] != 0)) {
        discarded.insert(goal);
      }
    }
    i += words;
  }
  // 2) pointers to those targets: the output variable and whatever is derived from it through
  // OpAccessChain.
  std::unordered_set<uint32_t> pointers;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (code == kOpVariable && words >= 4 && spirv[i + 3] == kStoreOutput) {
      const uint32_t id = spirv[i + 2];
      if (with_location.count(id) && !discarded.count(id)) {
        pointers.insert(id);
      }
    } else if ((code == kOpAccessChain || code == kOpInBoundsAccessChain) && words >= 4 &&
               pointers.count(spirv[i + 3])) {
      pointers.insert(spirv[i + 2]);
    }
    i += words;
  }
  if (pointers.empty()) {
    return {};
  }
  // 3) the writes to those pointers are dropped. The rest of the module is copied as is: the output
  // variable stays declared and in the entry point's interface, which is valid even if it is never written.
  std::vector<uint32_t> output;
  output.reserve(spirv.size());
  output.insert(output.end(), spirv.begin(), spirv.begin() + 5);
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    const bool outside = code == kOpStore && words >= 3 && pointers.count(spirv[i + 1]);
    if (outside) {
      ++removed;
    } else {
      output.insert(output.end(), spirv.begin() + i, spirv.begin() + i + words);
    }
    i += words;
  }
  (void)kOpEntryPoint;
  return removed ? output : std::vector<uint32_t>();
}

// Xenos k_2_10_10_10_FLOAT stores unsigned 7e3 RGB ([0, 31.875]) and two-bit UNORM alpha. The native
// renderer represents it with RGBA16F, whose wider range would let negative values, infinities and NaNs
// escape into later passes. Insert GLSL NClamp immediately before stores to the selected color outputs.
std::vector<uint32_t> LimitOutputs7e3(const std::vector<uint32_t>& spirv, uint32_t outputs_mask) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpExtInstImport = 11, kOpExtInst = 12, kOpTypeFloat = 22;
  constexpr uint32_t kOpTypeVector = 23, kOpConstant = 43, kOpConstantComposite = 44;
  constexpr uint32_t kOpFunction = 54, kOpVariable = 59, kOpStore = 62, kOpDecorate = 71;
  constexpr uint32_t kDecorationLocation = 30, kStoreOutput = 3, kNClamp = 81;
  if (!outputs_mask || spirv.size() < 5 || spirv[0] != kMagic) {
    return {};
  }
  uint32_t glsl = 0, float_type = 0, v_type4 = 0;
  size_t before_functions = 0;
  std::unordered_map<uint32_t, uint32_t> locations;
  std::unordered_set<uint32_t> outputs;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16, code = spirv[i] & 0xFFFF;
    if (!words || i + words > spirv.size()) return {};
    if (code == kOpExtInstImport && words >= 3 && !glsl) glsl = spirv[i + 1];
    if (code == kOpTypeFloat && words >= 3 && spirv[i + 2] == 32) float_type = spirv[i + 1];
    if (code == kOpTypeVector && words >= 4 && spirv[i + 2] == float_type && spirv[i + 3] == 4)
      v_type4 = spirv[i + 1];
    if (code == kOpDecorate && words >= 4 && spirv[i + 2] == kDecorationLocation)
      locations[spirv[i + 1]] = spirv[i + 3];
    if (code == kOpVariable && words >= 4 && spirv[i + 3] == kStoreOutput) {
      const auto l = locations.find(spirv[i + 2]);
      if (l != locations.end() && l->second < 4 && (outputs_mask & (1u << l->second)))
        outputs.insert(spirv[i + 2]);
    }
    if (code == kOpFunction && !before_functions) before_functions = i;
    i += words;
  }
  if (!glsl || !float_type || !v_type4 || !before_functions || outputs.empty()) return {};
  uint32_t next = spirv[3];
  const uint32_t zero = next++, one = next++, max_rgb = next++;
  const uint32_t min = next++, max = next++;
  std::vector<uint32_t> output;
  output.reserve(spirv.size() + 40);
  output.insert(output.end(), spirv.begin(), spirv.begin() + before_functions);
  const auto constant = [&](uint32_t id, uint32_t bits) {
    output.insert(output.end(), {(4u << 16) | kOpConstant, float_type, id, bits});
  };
  constant(zero, 0x00000000u);
  constant(one, 0x3F800000u);
  constant(max_rgb, 0x41FF0000u);  // 31.875, largest finite unsigned 7e3 value.
  output.insert(output.end(), {(7u << 16) | kOpConstantComposite, v_type4, min,
                               zero, zero, zero, zero});
  output.insert(output.end(), {(7u << 16) | kOpConstantComposite, v_type4, max,
                               max_rgb, max_rgb, max_rgb, one});
  uint32_t limited = 0;
  for (size_t i = before_functions; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16, code = spirv[i] & 0xFFFF;
    if (!words || i + words > spirv.size()) return {};
    if (code == kOpStore && words >= 3 && outputs.count(spirv[i + 1])) {
      const uint32_t limited_value = next++;
      output.insert(output.end(), {(8u << 16) | kOpExtInst, v_type4, limited_value, glsl,
                                   kNClamp, spirv[i + 2], min, max});
      output.push_back(spirv[i]);
      output.push_back(spirv[i + 1]);
      output.push_back(limited_value);
      ++limited;
    } else {
      output.insert(output.end(), spirv.begin() + i, spirv.begin() + i + words);
    }
    i += words;
  }
  if (!limited) return {};
  output[3] = next;
  return output;
}

/*
 * The same module, but declaring OpExecutionMode EarlyFragmentTests.
 *
 * In SPIR-V the execution modes have their own section, right after the OpEntryPoint instructions and
 * before the debug strings and decorations. So it is enough to insert the instruction after the module's
 * last OpExecutionMode (there is always at least one, OriginUpperLeft, which is what DXC emits). No new
 * ids are created, so the header's "bound" does not change and the module stays valid.
 *
 * Returns empty (and then the normal module is used) if the module has no fragment entry point, if it
 * already declared it, or if it writes gl_FragDepth (DepthReplacing): in that last case testing earlier
 * would change the result, because the Z being tested is computed by the shader itself.
 */
std::vector<uint32_t> WithEarlyTests(const std::vector<uint32_t>& spirv, const char*& reason) {
  constexpr uint32_t kMagic = 0x07230203u;
  constexpr uint32_t kOpEntryPoint = 15, kOpExecutionMode = 16;
  constexpr uint32_t kModelFragment = 4;
  constexpr uint32_t kEarlyMode = 9, kDepthModeReplacing = 12;
  reason = "";
  if (spirv.size() < 5 || spirv[0] != kMagic) {
    reason = "not SPIR-V";
    return {};
  }
  uint32_t entry = 0;
  size_t where = 0;  // first word after the execution mode section
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t words = spirv[i] >> 16;
    const uint32_t code = spirv[i] & 0xFFFF;
    if (words == 0 || i + words > spirv.size()) {
      reason = "malformed module";
      return {};
    }
    if (code == kOpEntryPoint && words >= 3) {
      if (spirv[i + 1] == kModelFragment) {
        if (entry) {
          reason = "more than one fragment entry point";
          return {};
        }
        entry = spirv[i + 2];
      }
      if (where < i + words) {
        where = i + words;
      }
    } else if (code == kOpExecutionMode && words >= 3) {
      if (spirv[i + 2] == kEarlyMode) {
        reason = "already declared";
        return {};
      }
      if (spirv[i + 2] == kDepthModeReplacing) {
        reason = "escribe gl_FragDepth";
        return {};
      }
      if (where < i + words) {
        where = i + words;
      }
    }
    i += words;
  }
  if (!entry || !where) {
    reason = "no fragment entry point";
    return {};
  }
  std::vector<uint32_t> output;
  output.reserve(spirv.size() + 3);
  output.insert(output.end(), spirv.begin(), spirv.begin() + where);
  output.push_back((3u << 16) | kOpExecutionMode);
  output.push_back(entry);
  output.push_back(kEarlyMode);
  output.insert(output.end(), spirv.begin() + where, spirv.end());
  return output;
}

struct PipelineKey {
  uint32_t vs = 0;
  uint32_t ps = 0;
  uint64_t entry = 0;
  uint32_t topology = 0;
  uint32_t specialization = 0;
  uint32_t formats[5] = {};
  uint32_t blend[4] = {};
  uint32_t masks = 0;
  uint32_t depth = 0;
  uint32_t rasterization = 0;
  // The key is hashed and compared byte by byte (PipelineFor). With 76 bytes of fields and 8-byte alignment
  // it had 4 bytes of uninitialized implicit padding: stack garbage that made identical keys differ and
  // created duplicate pipelines (125-134, and 193-203 in a later version, with no change in the image).
  uint32_t fill = 0;
  uint32_t fill2 = 0;
};
static_assert(std::has_unique_object_representations_v<PipelineKey>,
              "PipelineKey cannot have implicit padding: it is hashed and compared byte by byte");

/*
 * One pipeline of the prewarm list (masseffect_native_pipelines_prewarm).
 *
 * What is needed to recreate it in another session exactly as the ring created it: its key as PipelineFor
 * looks it up; the fingerprint of its two shaders in the library (if the library has changed, key.vs
 * and key.ps are no longer the same shaders and the record is skipped); and its vertex input, which the
 * key only carries as a fingerprint. It is stored on disk byte for byte, so it cannot have implicit
 * padding.
 */
struct AttributeRegister {
  uint32_t location = 0;
  uint32_t binding = 0;
  uint32_t format = 0;  // VkFormat
  uint32_t offset = 0;
};

struct RegisterPipeline {
  static constexpr uint32_t kMaxAttributes = 16;
  static constexpr uint32_t kMaxBindings = 16;
  PipelineKey key;
  uint64_t vs_fingerprint = 0;  // masseffect::native::Shader::fingerprint
  uint64_t ps_fingerprint = 0;  // 0 without a fragment stage (key.ps == 0)
  uint32_t n_attributes = 0;
  uint32_t n_bindings = 0;
  AttributeRegister attributes[kMaxAttributes] = {};
  uint32_t strides[kMaxBindings] = {};
};
static_assert(std::has_unique_object_representations_v<RegisterPipeline>,
              "RegisterPipeline is saved to disk byte by byte: no implicit padding");

struct Texture {
  ImageNative image;
  uint32_t layers = 1;  // 6 for cubemaps
  uint32_t background = 0;  // slices of 3D textures; 0 for the rest
  uint64_t fingerprint = 0;
  uint64_t frame = UINT64_MAX;
  uint64_t raw_fingerprint = 0;  // XXH3 of the guest bytes (2D and cubemaps)
  uint64_t next = 0;     // frame of the next check
  uint32_t interval = 1;     // frames between checks: 1 to 32
  uint8_t late_changes = 0;  // times a change was found after the texture had been stable (masseffect_native_adaptive_texture)
  uint32_t postponements = 0;  // consecutive checks deferred by the budget
  // Sample fingerprint (SampleFingerprint) of the same content as raw_fingerprint if valid_sample, and how many
  // consecutive rechecks have been accepted on it alone (masseffect_native_fingerprints_sampling).
  uint64_t sample_fingerprint = 0;
  bool valid_sample = false;
  uint8_t consecutive_samples = 0;
  bool needs_upload = false;
  std::vector<uint8_t> data;  // levels already laid out for the host: level after level and, in each, layer after layer
  uint32_t levels = 1;        // mip levels of the host image
  std::array<uint32_t, 16> level_displacement{};  // data bytes up to each level
  uint64_t bytes = 0;          // what it counts in bytes_textures_ (for eviction)
  // 1 + its index in in_flight_ while the bind thread runs its vkBindImageMemory; 0 = no. While set, the
  // image exists but has no memory: no command may reference it until CollectBindings.
  uint32_t in_flight = 0;
  // Measurement only (masseffect_native_diag_reuse): its shape without the address (when the image is
  // created), its address (only for the log), the content key it is filed under in live_per_content_
  // (0 = none) and whether it is new and its first fingerprint has not been measured yet.
  uint64_t shape_content = 0;
  uint64_t content_key = 0;
  uint32_t address = 0;
  bool content_per_measure = false;
};

struct View {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t slot = 0;
  uint32_t heap = 0;  // 0 2D textures, 2 cubemaps
};

class DrawsVulkanImpl final : public DrawsVulkan {
 public:
  DrawsVulkanImpl(const VulkanDevice* vulkan_device, rex::memory::Memory* memory,
                    ContextTargets* context)
      : vulkan_device_(vulkan_device),
        dfn_(deferred::Table(vulkan_device->functions())),
        device_(vulkan_device->device()),
        memory_(memory),
        context_(context) {}

  ~DrawsVulkanImpl() override {
    ReportPayloadAudit("shutdown");
    StopPrewarmed();  // uses the pipeline cache and the layout: first
    StopPreloadShaders();
    StopCopies();
    StopBindings();  // in-flight vkBindImageMemory calls finish before any image is destroyed
    SaveCachePipelines();
    StopWriterCache();  // writes whatever is still pending
    for (auto& [key, par] : pipelines_) {
      dfn_.vkDestroyPipeline(device_, par.second, nullptr);
    }
    if (cache_pipelines_ != VK_NULL_HANDLE) {
      destroy_cache_(device_, cache_pipelines_, nullptr);
    }
    for (auto& [key, framebuffer] : framebuffers_) {
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (VkFramebuffer framebuffer : fb_retired_) {  // the ones ForgetView retired
      dfn_.vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    for (auto& [key, pass] : passes_) {
      dfn_.vkDestroyRenderPass(device_, pass, nullptr);
    }
    for (auto& [entry, shader_module] : modules_) {
      dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
    }
    for (auto& [entry, shader_module] : modules_rectangle_) {
      if (shader_module) dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
    }
    for (const auto& [hash, bucket] : modules_depth_half_) {
      for (const auto& entry : bucket)
        if (entry.module) dfn_.vkDestroyShaderModule(device_, entry.module, nullptr);
    }
    for (const auto& [hash, bucket] : modules_fragcoord_xy_) {
      for (const auto& entry : bucket)
        if (entry.module) dfn_.vkDestroyShaderModule(device_, entry.module, nullptr);
    }
    for (const auto& [hash, bucket] : modules_depth_quantize_) {
      for (const auto& entry : bucket)
        if (entry.module) dfn_.vkDestroyShaderModule(device_, entry.module, nullptr);
    }
    for (auto& [entry, shader_module] : modules_alpha_only_) {
      if (shader_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
      }
    }
    for (auto& [entry, shader_module] : modules_z_early_) {
      if (shader_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
      }
    }
    for (auto& [key, shader_module] : modules_7e3_) {
      if (shader_module != VK_NULL_HANDLE) {
        dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
      }
    }
    for (auto& [key, view] : views_) {
      dfn_.vkDestroyImageView(device_, view.view, nullptr);
    }
    for (auto& [key, texture] : textures_) {
      DestroyImage(texture.image);
    }
    for (auto& [key, par] : samplers_) {
      dfn_.vkDestroySampler(device_, par.first, nullptr);
    }
    for (ImageNative& empty : empty_) {
      DestroyImage(empty);
    }
    // The pool's slabs are released after destroying every image that lives in them. The other way round
    // would free memory that the VkImages still have bound.
    pool_textures_.Finish();
    if (sampler_empty_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, sampler_empty_, nullptr);
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    if (pool_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pool_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, pool_ubo_, nullptr);
    if (layout_ubo_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout_ubo_, nullptr);
    for (VkDescriptorSetLayout layout : layouts_) {
      if (layout != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorSetLayout(device_, layout, nullptr);
    }
    for (const BufferUpload& s : shared_bufs_) {
      if (s.data) dfn_.vkUnmapMemory(device_, s.memory);
      if (s.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.buffer, nullptr);
      if (s.memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.memory, nullptr);
    }
    for (const BufferUpload& s : uploads_) {
      if (s.data) dfn_.vkUnmapMemory(device_, s.memory);
      if (s.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, s.buffer, nullptr);
      if (s.memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, s.memory, nullptr);
    }
  }

  bool Initialize() {
    const auto& properties = vulkan_device_->properties();
    const std::pair<bool, const char*> requirements[] = {
        {properties.shaderInt64, "shaderInt64"},
        {properties.bufferDeviceAddress, "bufferDeviceAddress"},
        {properties.runtimeDescriptorArray, "runtimeDescriptorArray"},
        {properties.shaderSampledImageArrayDynamicIndexing,
         "shaderSampledImageArrayDynamicIndexing"},
        {properties.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {properties.descriptorBindingSampledImageUpdateAfterBind,
         "descriptorBindingSampledImageUpdateAfterBind"},
        {properties.descriptorBindingUpdateUnusedWhilePending,
         "descriptorBindingUpdateUnusedWhilePending"},
    };
    for (const auto& [present, name] : requirements) {
      if (!present) {
        REXLOG_ERROR("[native] C6: the Vulkan device does not have {}: nothing is drawn", name);
        return false;
      }
    }
    buffer_address_ = reinterpret_cast<FnBufferAddress>(
        deferred::Proc(vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr, device_, "vkGetBufferDeviceAddress"));
    if (REXCVAR_GET(masseffect_native_gpu_labels) &&
        vulkan_device_->vulkan_instance()->extensions().ext_EXT_debug_utils) {
      label_gpu_ = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
          deferred::Proc(vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr, device_, "vkCmdInsertDebugUtilsLabelEXT"));
      REXLOG_INFO("[native] ME GPU draw labels: {}", label_gpu_ ? "enabled" : "unavailable");
    }
    LoadCachePipelines();
    LoadDynamicState();  // dynamic state phases 1 and 2
    if (!buffer_address_ || !CreateUpload() || !CreateDescriptors()) {
      return false;
    }
    // The pool is created after CreateDescriptors (where textures_mb_max_ is read) and before CreateEmpty,
    // so the three empty images can already come from it. The slabs are prewarmed here, while the game is
    // still loading: a 32 MB memset during play would be a 10-15 ms stutter.
    pool_textures_.Start(vulkan_device_, textures_mb_max_);
    return CreateEmpty();
  }

  bool Draw(const SubmissionDraw& p) override {
    // Stage stopwatch on 1 in kStopwatchEach draws: reading the clock 12 times per draw took almost a
    // quarter of the ring thread's busy CPU on PC, and on the Switch each read costs more. The C6 averages
    // come from the sample.
    time_ = (++stopwatch_counter_ & (kStopwatchEach - 1)) == 0;
    auto mark = time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const uint32_t* r = p.registers;
    if (!r || !p.vs) {
      return false;
    }
    if (ResolveFetchAuditEnabled()) {
      me_resolve_fetch_vs_ = p.vs->number;
      me_resolve_fetch_ps_ = p.ps ? int(p.ps->number) : -1;
    }
    if (PayloadAuditEnabled()) {
      me_payload_vs_ = p.vs->number;
      me_payload_ps_ = p.ps ? int(p.ps->number) : -1;
      for (uint32_t info : {gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
                            gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO}) {
        const uint32_t format = (r[info] >> 16) & 15;
        if (format == 3 || format == 7 || format == 12) me_payload_hdr_ = true;
      }
    }
    // --- Primitive type ---------------------------------------------------
    const uint32_t edram_mode = r[gr::XE_GPU_REG_RB_MODECONTROL] & 0x7;
    if (edram_mode != uint32_t(xenos::EdramMode::kColorDepth) && edram_mode != 5) {
      return Reject(1, "EDRAM mode without color or depth");
    }
    // Mode 5 (depth only): the Xenos does not run the pixel shader (IsPixelShaderNeededWithRasterization). Without a PS there are no textures, pixel constants or alpha test,
    // and the pipeline has no fragment stage. The shadow passes work this way, and some arrive with the PS
    // object set to 0.
    const ShaderEntry* ps =  // can be removed if it writes no color and does not discard
        edram_mode == uint32_t(xenos::EdramMode::kColorDepth) ? p.ps : nullptr;
    if (!ps && edram_mode == uint32_t(xenos::EdramMode::kColorDepth)) {
      return false;
    }
    const uint32_t starter = r[gr::XE_GPU_REG_VGT_DRAW_INITIATOR];
    const uint32_t type = starter & 0x3F;
    const uint32_t src_data = (starter >> 6) & 0x3;
    const bool indices32 = (starter >> 11) & 0x1;
    uint32_t count = starter >> 16;
    if (!count) {
      return true;
    }
    VkPrimitiveTopology topology;
    bool quads = false;
    bool fan = false;
    bool accepts_reset = false;
    switch (type) {
      case 1:
        // Mass Effect uses point lists in its D3D utility passes. Vulkan accepts the base topology;
        // point-sprite expansion (when enabled by raster state) is handled separately from topology.
        topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        break;
      case 2:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        break;
      case 3:
        topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        accepts_reset = true;
        break;
      case 4:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        break;
      case 5:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        fan = true;
        accepts_reset = true;
        break;
      case 6:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        accepts_reset = true;
        break;
      case 8:
        // Expand the three real post-VS corners into six host vertices below.
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        break;
      case 13:
        topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        quads = true;
        break;
      default:
        return Reject(100 + type, "primitive type not supported yet");
    }
    if (src_data == uint32_t(xenos::SourceSelect::kImmediate)) {
      return Reject(2, "immediate indices");
    }
    // Clipping disabled on the Xenos: the position may come in pixels (videos). It is drawn with a viewport
    // the size of the render target and the transform in the VS, like the emulation.
    const bool no_clip = (r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 16) & 0x1;
    const uint32_t sc_mode = r[gr::XE_GPU_REG_PA_SU_SC_MODE_CNTL];
    if (((sc_mode >> 3) & 0x3) == 2 && ((sc_mode >> 5) & 0x7) != 2) {
      return Reject(4, "polygons drawn as points or lines");
    }

    const VerticesEntry* entry = EntryFor(p);
    if (!entry) {
      return false;
    }
    Stage(0, mark);
    std::chrono::steady_clock::time_point t_indices_185 = mark;  // C6 substages 15-18

    // --- Targets --------------------------------------------------------------
    const uint32_t pitch = r[gr::XE_GPU_REG_RB_SURFACE_INFO] & 0x3FFF;
    // B1 (black shards): the scene's depth prepass is drawn into the 2x view and its depth, brought back to
    // the 1x view, differs from the material pass's own depth on slopes; the material pass's depth test then
    // drops whole triangles. The material pass writes its own depth, so the prepass (depth-only draws at the
    // screen width, not rectangle clears) is skipped.
    static const uint32_t screen_width = uint32_t(rex::cvar::HasNonDefaultValue("video_mode_width") ?
        std::max<int32_t>(1, std::atoi(rex::cvar::GetFlagByName("video_mode_width").c_str())) : 1280);
    // (A smaller guest video mode keeps the 1280 surface pitch and narrows only the viewport.)
    if (edram_mode == 5 && (pitch == 1280 || pitch == screen_width) && type != 8 &&
        REXCVAR_GET(masseffect_native_skip_prepass)) {
      return true;
    }
    const uint32_t register_mask =
        edram_mode == uint32_t(xenos::EdramMode::kColorDepth) ? r[gr::XE_GPU_REG_RB_COLOR_MASK] : 0;
    static constexpr uint32_t kInfoColor[4] = {
        gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
        gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO};
    uint64_t keys[5] = {};
    uint32_t masks = 0;
    bool has_target = false;
    for (uint32_t i = 0; i < 4; ++i) {
      const uint32_t mask = (register_mask >> (i * 4)) & 0xF;
      if (!mask || !ps || !((ps->outputs >> i) & 0x1)) {
        continue;
      }
      const uint32_t info = r[kInfoColor[i]];
      const uint32_t format = (info >> 16) & 0xF;
      if (HostFormatTargetColor(format) == VK_FORMAT_UNDEFINED) {  // Mass Effect: HDR targets
        return Reject(200 + format, "color target format not supported yet");
      }
      keys[i] = (uint64_t(1) << 63) | (uint64_t(info & 0xFFF) << 24) | (uint64_t(format) << 16) |
                  (uint64_t((r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3) << 40) | pitch;
      masks |= mask << (i * 4);
      has_target = true;
    }
    /*
     * Draws that cannot change a single pixel (masseffect_native_skip_invisibles).
     *
     * Two cases, read from this draw's own registers without any heuristics:
     *
     *  - Blending is "0 x source + 1 x destination" (ADD) on the written channels: the result is the
     *    destination as is. It is checked per channel group because the mask may write only RGB or only
     *    alpha, and then the other group's factors do not matter.
     *  - The alpha test function is 0 = NEVER: alphaTestValue (shader_common.h) returns -1 and clip() kills
     *    every fragment, before depth is written.
     *
     * If there is also no depth or stencil left to write, the whole draw is unnecessary: it is dropped. If
     * it writes depth it is drawn anyway and only counted, because setting the color mask to 0 would send it
     * down the "no color" path (kSpecAlphaOnly), where masseffect_shadows_no_vegetation drops the whole draw and
     * its depth would be lost. Removing the color write saves too little to be worth it.
     */
    if (skip_invisibles_ && masks) {
      static constexpr uint32_t kBlendFor[4] = {
          gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
          gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
      const uint32_t control_color_inv = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool alpha_never =
          ((control_color_inv >> 3) & 0x1) && (control_color_inv & 0x7) == 0;  // function NEVER
      uint32_t useful_masks = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t i_mask = (masks >> (i * 4)) & 0xF;
        if (!i_mask) {
          continue;
        }
        const uint32_t m = r[kBlendFor[i]] & 0x1FFF1FFF;
        const bool copy_color = (m & 0x1F) == 0 && ((m >> 8) & 0x1F) == 1 && ((m >> 5) & 0x7) == 0;
        const bool copy_alpha =
            ((m >> 16) & 0x1F) == 0 && ((m >> 24) & 0x1F) == 1 && ((m >> 21) & 0x7) == 0;
        const bool writes_rgb = (i_mask & 0x7) != 0 && !copy_color;
        const bool writes_alpha = (i_mask & 0x8) != 0 && !copy_alpha;
        if (writes_rgb || writes_alpha) {
          useful_masks |= i_mask << (i * 4);
        }
      }
      if ((!useful_masks || alpha_never)) {
        // With the NEVER function even the depth write dies (clip() runs in the shader, before the merger),
        // so the draw leaves no trace of any kind. With the blending that copies the destination, the draw can
        // only be dropped if it writes neither Z nor stencil either.
        const uint32_t dc = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
        const bool leaves_trail =
            !alpha_never && ((((dc >> 1) & 0x1) && ((dc >> 2) & 0x1)) || (dc & 0x1));
        ++counts_z_[alpha_never ? kInvisibleAlpha : kInvisibleBlend];
        if (!leaves_trail) {
          return true;
        }
        ++counts_z_[kInvisibleColorOnly];  // drawn anyway: only counts what is left to gain
      }
    }
    // With no color to write, the pixel shader can only have an effect by discarding pixels (alpha test or
    // its own kill) or by writing depth. If it does neither, the pipeline has no fragment stage and the image
    // is identical: the GPU saves shading the whole shadow map.
    // Of the draws that do write color, how many can use early depth rejection and how many force shading
    // before testing. That is what decides whether a depth pre-pass would help at all.
    if (masks && ps && has_target) {
      const uint32_t control_color_now = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool test_alpha =
          ((control_color_now >> 3) & 0x1) && (control_color_now & 0x7) != 7;
      const bool discards = test_alpha || ps->discards || (ps->outputs & 0x10);
      if (pitch >= 1600) {
        // shadow map: already counted separately
      } else if (discards) {
        ++scene_with_discard_;
      } else {
        ++scene_no_discard_;
      }
    }
    // Shadow map detection: depth-only targets matching shadow dimensions (880x880, 1280x1280, 1600x1600).
    const bool target_shadows =  // also used for its depth bias (verified rule: pitch >= 1600)
        pitch >= 1600 && keys[4] && !keys[0] && !keys[1] && !keys[2] && !keys[3];
    bool alpha_only = false;
    if (!masks && ps) {
      const uint32_t control_color_now = r[gr::XE_GPU_REG_RB_COLORCONTROL];
      const bool test_alpha =
          ((control_color_now >> 3) & 0x1) && (control_color_now & 0x7) != 7;
      const bool ps_writes_z = (ps->outputs & 0x10) != 0;
      if (!test_alpha && !ps->discards && !ps_writes_z) {
        ++draws_ps_useless_;
        if (cvars_per_frame_ ? no_ps_no_color_frame_ : NoPsNoColor()) {
          ps = nullptr;
        }
      } else {
        ++draws_ps_required_;
        // Diagnostic: in the shadow map the pixel shader reads constant c1 (g_bShadowMapAlphaEnabled). With
        // that constant at 0 it neither samples nor discards: the stage would be unnecessary.
        if (pitch >= 1600) {
          (Float(r[kRegConstantsPs + 4]) != 0.0f ? ++shadows_active_alpha_ : ++shadows_off_alpha_);
        }
        // Needed for the alpha test or a kill, but its color goes nowhere: it is compiled without those
        // writes.
        if (cvars_per_frame_ ? ps_alpha_only_frame_ : PsAlphaOnly()) {
          alpha_only = true;
        }
      }
    }
    /*
     * Shadow map vegetation discard, as early as possible.
     *
     * This same discard used to live 640 lines further down, right before the pipeline lookup. So these
     * draws were paid in full and then thrown away: index conversion, vertex sources, PrepareTexture for all
     * their samplers, upload buffer allocation, pass change, vertex copy, index and constant memcpy,
     * viewport, scissor and pipeline lookup.
     *
     * And it is not a handful of draws: 604 per frame are dropped, 31 % of those that come in (the
     * `draws_ps_required` counter matches the lost ones exactly across twelve intervals). At
     * ~8.5 us per incoming draw, that is 3-5 ms.
     *
     * Careful when measuring this: the divisor of `C6 stages` is the recorded draws (~1,370), not the
     * incoming ones (~1,970), even though the label says otherwise. Multiplying by the incoming ones inflates
     * the budget by 45 %.
     */
    const bool early_vegetation = alpha_only && no_vegetation_;
    if (early_vegetation) {
      ++draws_vegetation_soon_;
      return true;
    }
    const uint32_t control_depth = r[gr::XE_GPU_REG_RB_DEPTHCONTROL];
    if (control_depth & 0x3) {  // stencil or z
      const uint32_t info = r[gr::XE_GPU_REG_RB_DEPTH_INFO];
      keys[4] = (uint64_t(1) << 62) | (uint64_t(info & 0xFFF) << 24) |
                  (uint64_t((info >> 16) & 0x1) << 16) |
                  (uint64_t((r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3) << 40) | pitch;
      has_target = true;
    }
    // Match the SDK's same-base binding priority. ME1's lighting shaders may
    // declare two outputs at one physical base; binding the same VkImage twice
    // exposes undefined unwritten-output data. Lower color slot wins, color
    // wins over depth (RenderTargetCache::Update, ME1 4D5307E6 comment).
    me::native::EdramBoundRequest bound_request;
    bound_request.pitch_pixels = pitch;
    bound_request.msaa = (r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3;
    bound_request.depth_used = keys[4] != 0;
    bound_request.normalized_color_mask = uint16_t(masks);
    bound_request.views[0].base = uint32_t(keys[4] >> 24) & 0xFFF;
    for (uint32_t i = 0; i < 4; ++i)
      bound_request.views[i + 1].base = uint32_t(keys[i] >> 24) & 0xFFF;
    const auto bound_plan = me::native::PlanEdramBoundRanges(bound_request);
    if (bound_plan.valid && bound_plan.suppressed_mask) {
      if (bound_plan.suppressed_mask & 1) keys[4] = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        if (!(bound_plan.suppressed_mask & (1u << (i + 1)))) continue;
        keys[i] = 0;
        masks &= ~(0xFu << (4 * i));
      }
      if (++me_bound_collisions_ <= 8)
        REXLOG_INFO("[native] EDRAM same-base binding: requested {:02X}, active {:02X}, VS n{} PS n{}",
                    bound_plan.requested_mask, bound_plan.active_mask, p.vs->number,
                    ps ? int(ps->number) : -1);
      has_target = bound_plan.active_mask != 0;
    }
    if (!has_target || !pitch) {
      return true;  // writes nothing visible
    }
    CutSubstage(15, t_indices_185);  // render targets and discards
    // --- Vertex and index range --------------------------------------------
    const uint32_t displacement = r[gr::XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
    const bool reset = accepts_reset && ((sc_mode >> 21) & 0x1);
    // Depth bias as in the emulation with host render targets (GetPreferredFacePolygonOffset).
    float scale_bias = 0.0f, bias_displacement = 0.0f;
    if (type >= 4 && type != 8) {  // rectangles use the parallelogram state, not polygon faces
      if (((sc_mode >> 11) & 0x1) && !(sc_mode & 0x1)) {
        scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
        bias_displacement = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
      }
      if (((sc_mode >> 12) & 0x1) && !((sc_mode >> 1) & 0x1) && scale_bias == 0.0f &&
          bias_displacement == 0.0f) {
        scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE]);
        bias_displacement = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET]);
      }
    } else if ((sc_mode >> 13) & 0x1) {
      scale_bias = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE]);
      bias_displacement = Float(r[gr::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET]);
    }
    // Constant scaled by the guest format's minimum value (2^24-1 for D24S8, 2^24 for D24FS8) and slope
    // from subpixels to pixels.
    float bias[2] = {
        bias_displacement * (((r[gr::XE_GPU_REG_RB_DEPTH_INFO] >> 16) & 0x1)
                                    ? float(uint32_t(1) << 24)
                                    : float((uint32_t(1) << 24) - 1)),
        scale_bias * (1.0f / 16.0f)};
    // masseffect_native_shadows_pending_bias, only in the shadow map and only if the game does not set its own.
    if (target_shadows && type >= 4 && bias[0] == 0.0f && bias[1] == 0.0f && shadows_pending_bias_ != 0) {
      bias[1] = float(shadows_pending_bias_) * 0.1f;
    }
    const bool with_bias = bias[0] != 0.0f || bias[1] != 0.0f;
    if (with_bias && warnings_bias_ < 8 &&
        !Equal(bias, warned_bias_, sizeof(bias))) {
      ++warnings_bias_;
      std::memcpy(warned_bias_, bias, sizeof(bias));
      REXLOG_INFO("[native] C6: depth bias: scale {} offset {} "
                  "(mode {:08X}) -> constant {} slope {}",
                  scale_bias, bias_displacement, sc_mode, bias[0], bias[1]);
    }
    CutSubstage(16, t_indices_185);  // depth bias
    uint32_t vmin = UINT32_MAX;
    uint32_t vmax = 0;
    bool with_indices = false;
    bool indices_from_16 = false;  // in indices16_ instead of indices_
    // masseffect_native_indices_cache (see the fast path below).
    bool indices_cache_hit = false, indices_cache_registrar = false;
    uint64_t indices_cache_offset = 0, indices_cache_frame_hit = 0, indices_cache_key = 0;
    uint64_t indices_cache_fingerprint = 0;
    const uint8_t* indices_cache_data = nullptr;
    bool indices_cache_rotate = false;
    indices_.clear();
    if (src_data == uint32_t(xenos::SourceSelect::kDMA)) {
      const uint32_t size = r[gr::XE_GPU_REG_VGT_DMA_SIZE];
      const auto order = static_cast<xenos::Endian>(size >> 30);
      const uint32_t bytes = indices32 ? 4 : 2;
      const uint32_t base = r[gr::XE_GPU_REG_VGT_DMA_BASE] & ~(bytes - 1);
      if (count > (size & 0xFFFFFF) || uint64_t(base & 0x1FFFFFFF) + uint64_t(count) * bytes >
                                               kPhysicalMemory) {
        if (warnings_indices_ < 8) {
          ++warnings_indices_;
          REXLOG_WARN("[native] C6 diag: indices: count {} words {} VGT_DMA_SIZE {:08X} "
                      "VGT_DMA_BASE {:08X} 32-bit {} type {} VS n{} PS n{}",
                      count, size & 0xFFFFFF, size, r[gr::XE_GPU_REG_VGT_DMA_BASE], indices32,
                      type, p.vs->number, ps ? int(ps->number) : -1);
        }
        return Reject(7, "indices outside their buffer");
      }
      const uint8_t* data = memory_->TranslatePhysical(base & 0x1FFFFFFF);
      const uint32_t reset_index = r[gr::XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
      if (!indices32 && !reset && !displacement && !quads && !fan &&
          (order == xenos::Endian::k8in16 || order == xenos::Endian::kNone)) {
        // The normal case in MASSEFFECT: 16 bits without restart or offset. They are uploaded as 16 bits, with a
        // branch-free loop the compiler can vectorize.
        indices16_.resize(count);
        uint16_t* output = indices16_.data();
        uint32_t min = 0xFFFF;
        uint32_t max = 0;
        if (indices_cache_active_ && type != 8) {
          const uint32_t sinc = g_synchronizations_ring.load(std::memory_order_relaxed);
          if (sinc != indices_cache_sinc_view_) {  // the guest may have rewritten a range it already drew
            indices_cache_sinc_view_ = sinc;
            ++indices_cache_generation_;
          }
          indices_cache_data = data;
          indices_cache_rotate = order == xenos::Endian::k8in16;
          indices_cache_key = (uint64_t(base & 0x1FFFFFFF) << 2) | (indices_cache_rotate ? 1u : 0u);
          const uint64_t bytes_i = uint64_t(count) * 2;
          indices_cache_fingerprint = bytes_i <= 16384 ? std::max<uint64_t>(1, RangeFingerprint(data, size_t(bytes_i))) : 0;
          IndicesEntryCache& e = indices_cache_[(indices_cache_key * 0x9E3779B97F4A7C15ull >> 40) &
                                                  (indices_cache_.size() - 1)];
          if (e.frame == frame_ && e.generation == indices_cache_generation_ &&
              e.key == indices_cache_key && e.count == count && e.fingerprint == indices_cache_fingerprint) {
            min = e.vmin;
            max = e.vmax;
            indices_cache_hit = true;
            indices_cache_offset = e.offset;
            indices_cache_frame_hit = frame_;
            ++indices_cache_hits_;
            indices_cache_bytes_saved_ += bytes_i;
            if (indices_cache_hits_ % 50000 == 0) {
              REXLOG_INFO("[native] ME index cache: {} hits, {:.0f} MB neither converted nor copied, {} lost "
                          "due to buffer changes",
                          indices_cache_hits_, double(indices_cache_bytes_saved_) / 1048576.0,
                          indices_cache_lost_);
            }
          } else {
            indices_cache_registrar = true;
          }
        }
        if (!indices_cache_hit) {
          // With hand-written NEON and its guard (IndicesFrom16, masseffect_native_indices_neon).
          IndicesFrom16(data, count, output, order == xenos::Endian::k8in16, min, max);
        }
        vmin = min;
        vmax = max;
        indices_from_16 = true;
      } else {
        indices_.resize(count);
        uint32_t* output = indices_.data();
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t v;
          if (indices32) {
            uint32_t raw_bits;
            std::memcpy(&raw_bits, data + size_t(i) * 4, 4);
            v = xenos::GpuSwap(raw_bits, order) & 0xFFFFFF;
          } else {
            uint16_t raw_bits;
            std::memcpy(&raw_bits, data + size_t(i) * 2, 2);
            v = xenos::GpuSwap(raw_bits, order);
          }
          if (reset && v == reset_index) {
            output[i] = UINT32_MAX;
            continue;
          }
          v = (v + displacement) & 0xFFFFFF;
          vmin = std::min(vmin, v);
          vmax = std::max(vmax, v);
          output[i] = v;
        }
      }
      if (vmin > vmax) {
        return true;  // resets only
      }
      with_indices = true;
    } else {
      vmin = displacement;
      vmax = displacement + count - 1;
      if (quads || fan) {
        indices_.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
          indices_[i] = displacement + i;
        }
        with_indices = true;
      }
    }
    if (vmax - vmin >= kMaxVerticesPerDraw) {
      return Reject(8, "vertex range too large");
    }
    if (quads) {
      const size_t n = indices_.size() / 4;
      converted_.resize(n * 6);
      for (size_t q = 0; q < n; ++q) {
        const uint32_t* i = &indices_[q * 4];
        uint32_t* o = &converted_[q * 6];
        o[0] = i[0]; o[1] = i[1]; o[2] = i[2];
        o[3] = i[0]; o[4] = i[2]; o[5] = i[3];
      }
      indices_.swap(converted_);
    }
    if (fan) {
      me::native::ConvertFan(indices_, converted_);
      indices_.swap(converted_);
      if (indices_.empty()) return true;
    }
    CutSubstage(17, t_indices_185);  // indices
    // The indices stay as they come: vkCmdDrawIndexed subtracts vmin through vertexOffset.
    const uint32_t vertices = vmax - vmin + 1;

    // Bytes of each vertex binding in guest memory.
    struct Source {
      const uint8_t* data;
      xenos::Endian order;
      uint32_t bytes;
      uint64_t address;  // physical, of the first byte used
    };
    std::array<Source, 16> sources{};
    if (entry->bindings.size() > sources.size()) {
      return Reject(9, "too many vertex streams");
    }
    VkDeviceSize bytes_vertices = 0;
    for (size_t b = 0; b < entry->bindings.size(); ++b) {
      const BindingVertices& binding = entry->bindings[b];
      const uint32_t d0 = r[kRegFetch + binding.slot * 2];
      const uint32_t d1 = r[kRegFetch + binding.slot * 2 + 1];
      if ((d0 & 0x3) != uint32_t(xenos::FetchConstantType::kVertex)) {
        return Reject(10, "invalid vertex fetch constant");
      }
      const uint64_t address = uint64_t(d0 & 0x1FFFFFFC);
      const uint64_t available = uint64_t((d1 >> 2) & 0xFFFFFF) * 4;
      const uint64_t begin = uint64_t(vmin) * binding.stride;
      uint64_t required = uint64_t(vertices) * binding.stride;
      if (begin + required > available) {
        Warn(11, "vertices beyond the end of their buffer: clipped");
        required = available > begin ? (available - begin) / binding.stride * binding.stride : 0;
      }
      if (!required || address + begin + required > kPhysicalMemory) {
        return Reject(12, "vertices outside memory");
      }
      sources[b] = {memory_->TranslatePhysical(uint32_t(address + begin)),
                     static_cast<xenos::Endian>(d1 & 0x3), uint32_t(required),
                     address + begin};
      bytes_vertices += (required + 3) & ~VkDeviceSize(3);
    }

    VerticesEntry rectangle_entry;
    std::array<std::vector<uint8_t>, 16> data_rectangle;
    static const bool audit_depth_rectangle = [] {
      const char* value = std::getenv("MASSEFFECT_NATIVE_AUDIT_DEPTH_RECTANGLE");
      return value && *value == '1';
    }();
    if (audit_depth_rectangle && me_constant_audit_hdr_ && type == 8 &&
        !masks && (control_depth & 0x76) == 0x76 &&
        me_depth_rectangle_audits_ < 16) {
      ++me_depth_rectangle_audits_;
      REXLOG_INFO("[native] depth rectangle audit {} VS n{} PS n{} pitch {} MSAA {} depthinfo {:08X} "
                  "stencil {:08X} clip {:08X} VTE {:08X} viewportZ {:08X}/{:08X}",
                  me_depth_rectangle_audits_, p.vs->number, ps ? int(ps->number) : -1,
                  pitch, (r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3,
                  r[gr::XE_GPU_REG_RB_DEPTH_INFO], r[gr::XE_GPU_REG_RB_STENCILREFMASK],
                  r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], r[gr::XE_GPU_REG_PA_CL_VTE_CNTL],
                  r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE], r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]);
      for (const AttributeVertices& a : entry->attributes) {
        const Source& origin = sources[a.binding];
        const uint32_t stride = entry->bindings[a.binding].stride;
        for (uint32_t v = 0; v < std::min(vertices, 3u); ++v) {
          const uint64_t start = uint64_t(v) * stride + a.offset;
          uint32_t words[4]{};
          if (start < origin.bytes) {
            std::memcpy(words, origin.data + start,
                        std::min<uint64_t>(sizeof(words), origin.bytes - start));
            for (auto& word : words) word = xenos::GpuSwap(word, origin.order);
          }
          REXLOG_INFO("[native] depth rectangle attr {} format {} stride {} offset {} v{} "
                      "words {:08X} {:08X} {:08X} {:08X} asfloat {} {} {} {}",
                      a.location, uint32_t(a.format), stride, a.offset, v,
                      words[0], words[1], words[2], words[3], Float(words[0]),
                      Float(words[1]), Float(words[2]), Float(words[3]));
        }
      }
    }
    if (type == 8) {
      auto [it, inserted] = shaders_rectangle_.try_emplace(p.vs);
      if (inserted) it->second = me::native::ExpandRectangleShader(p.vs->shader->Spirv());
      const auto& shader = it->second;
      if (shader.words.empty()) return Reject(108, "rectangle VS interface unsupported");
      rectangle_entry = *entry;
      rectangle_entry.attributes.clear();
      for (const auto& mapping : shader.inputs) {
        const auto attr = std::find_if(entry->attributes.begin(), entry->attributes.end(),
            [&](const AttributeVertices& a) { return a.location == mapping[0]; });
        if (attr == entry->attributes.end()) return Reject(108, "rectangle missing VS input");
        for (uint32_t c = 0; c < 3; ++c) {
          auto a = *attr; a.location = mapping[c+1];
          a.offset += c * entry->bindings[a.binding].stride;
          rectangle_entry.attributes.push_back(a);
        }
      }
      const uint32_t rectangles = count / 3;
      if (!rectangles) {
        return true;
      }
      bytes_vertices = 0;
      for (size_t b = 0; b < entry->bindings.size(); ++b) {
        const uint32_t stride = entry->bindings[b].stride;
        const uint64_t bytes = uint64_t(rectangles) * 6 * 3 * stride;
        if (bytes > kUploadSize || stride > 2048 / 3)
          return Reject(108, "rectangle expanded stream exceeds limits");
        auto& data = data_rectangle[b]; data.resize(size_t(bytes));
        for (uint32_t q = 0; q < rectangles; ++q) {
          for (uint32_t c = 0; c < 3; ++c) {
            const uint32_t guest_index = with_indices
                ? (indices_from_16 ? indices16_[q*3+c] : indices_[q*3+c]) : displacement + q*3+c;
            const uint64_t offset = uint64_t(guest_index - vmin) * stride;
            if (guest_index < vmin || offset + stride > sources[b].bytes)
              return Reject(108, "rectangle corner outside stream");
            for (uint32_t v = 0; v < 6; ++v)
              std::memcpy(data.data() + (size_t(q)*18 + v*3 + c)*stride,
                          sources[b].data + offset, stride);
          }
        }
        sources[b].data = data.data(); sources[b].bytes = uint32_t(bytes);
        rectangle_entry.bindings[b].stride = stride * 3;
        bytes_vertices += bytes;
      }
      rectangle_entry.fingerprint = XXH3_64bits(rectangle_entry.attributes.data(),
          rectangle_entry.attributes.size()*sizeof(AttributeVertices));
      entry = &rectangle_entry;
      count = rectangles * 6; with_indices = false; indices_from_16 = false;
      indices_.clear(); indices16_.clear(); vmin = 0; vmax = count - 1;
      if (recorded_rectangles_.insert(p.vs->number).second)
        REXLOG_INFO("[native] rectangle post-VS expansion: VS n{}, {} attributes, {} rectangles",
                    p.vs->number, shader.inputs.size(), rectangles);
      if (rectangles_recorded_constants_.insert(p.vs->number).second) {
        for (uint32_t reg = 252; reg < 256; ++reg)
          REXLOG_INFO("[native] rectangle VS n{} guest c{} = {:08X} {:08X} {:08X} {:08X}",
                      p.vs->number, reg, r[kRegConstantsVs+reg*4], r[kRegConstantsVs+reg*4+1],
                      r[kRegConstantsVs+reg*4+2], r[kRegConstantsVs+reg*4+3]);
      }
    }
    CutSubstage(18, t_indices_185);  // vertices and diagnostics
    Stage(1, mark);
    // --- Textures and samplers ----------------------------------------------------
    me_resolved_fetch_failed_this_draw_ = false;
    uint32_t shared[kSharedWords] = {};
    VkDeviceSize bytes_textures = 0;
    textures_to_upload_.clear();
    const auto t_samplers =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (const SamplerShader& sampler :
         ps ? std::span<const SamplerShader>(ps->samplers) : std::span<const SamplerShader>()) {
      if (sampler.register_value >= 16) {
        continue;
      }
      const uint32_t* fetch = r + kRegFetch + uint32_t(sampler.register_value) * 6;
      ++samplers_prepared_;
      // Per-register cache: same fetch constant, same frame and no render target copies in between.
      CacheSampler& cache = cache_samplers_[sampler.register_value];
      // Valid until cache.valid_until (with the cross-frame cache disabled, only its own frame)
      if ((cache_between_frames_ ? frame_ <= cache.valid_until : cache.frame == frame_) &&
          cache.generation == generation_textures_ &&
          Equal(cache.fetch.data(), fetch, sizeof(cache.fetch))) {
        shared[cache.heap * 16 + sampler.register_value] = cache.slot;
        shared[48 + sampler.register_value] = cache.sampler;
        WriteInvSize(shared, sampler.register_value, cache.width, cache.height);
        ++samplers_cache_;
        continue;
      }
      uint32_t slot_texture = 0;
      uint32_t heap = 0;
      uint32_t slot_sampler = 0;
      uint32_t host_width = 0, host_height = 0;
      // Second cache, keyed by the whole fetch constant within the same frame and generation: a register
      // changes texture between draws, but textures repeat a lot within a frame, and PrepareTexture already
      // queued their upload the first time.
      CacheSampler& per_fetch =
          cache_fetch_[XXH3_64bits(fetch, sizeof(uint32_t) * 6) & (cache_fetch_.size() - 1)];
      uint64_t valid_until = frame_;
      if ((cache_between_frames_ ? frame_ <= per_fetch.valid_until : per_fetch.frame == frame_) &&
          per_fetch.generation == generation_textures_ &&
          Equal(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch))) {
        slot_texture = per_fetch.slot;
        heap = per_fetch.heap;
        slot_sampler = per_fetch.sampler;
        valid_until = per_fetch.valid_until;
        host_width = per_fetch.width;
        host_height = per_fetch.height;
        ++samplers_cache_fetch_;
      } else {
        // Why the table misses ("C6 cache per fetch" report, every 10 s).
        if (!Equal(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch))) {
          ++(per_fetch.frame == UINT64_MAX ? fetch_empty_failures_ : fetch_failures_clash_);
        } else if (per_fetch.generation != generation_textures_) {
          ++fetch_failures_generation_;
        } else {
          ++fetch_expired_failures_;
        }
        bool punctual_sampling = false;
        me_resolve_fetch_sampler_ = sampler.register_value;
        PrepareTexture(fetch, slot_texture, heap, bytes_textures, punctual_sampling, valid_until,
                        host_width, host_height);
        if (me_resolved_fetch_failed_this_draw_) {
          return false;  // No cache entry and no stale-RAM / empty-texture fallback.
        }
        slot_texture |= uint32_t(RemappedSigns(fetch)) << 24;
        slot_sampler = SlotSampler(fetch, punctual_sampling);
        per_fetch.frame = frame_;
        per_fetch.generation = generation_textures_;
        std::memcpy(per_fetch.fetch.data(), fetch, sizeof(per_fetch.fetch));
        per_fetch.slot = slot_texture;
        per_fetch.heap = heap;
        per_fetch.sampler = slot_sampler;
        per_fetch.valid_until = valid_until;
        per_fetch.width = host_width;
        per_fetch.height = host_height;
      }
      shared[heap * 16 + sampler.register_value] = slot_texture;
      shared[48 + sampler.register_value] = slot_sampler;
      WriteInvSize(shared, sampler.register_value, host_width, host_height);
      cache.frame = frame_;
      cache.generation = generation_textures_;
      std::memcpy(cache.fetch.data(), fetch, sizeof(cache.fetch));
      cache.slot = slot_texture;
      cache.heap = heap;
      cache.sampler = slot_sampler;
      cache.valid_until = valid_until;
      cache.width = host_width;
      cache.height = host_height;
    }
    if (time_) {  // C6 substages: the sampler loop inside the textures stage
      sub_ns_[14] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_samplers)
                                  .count());
      ++sub_n_[14];
    }
    Stage(2, mark);
    // Upload commands execute BEFORE all work commands in the submission. Updating
    // an existing image here would retroactively change earlier draws sampling it:
    // a transfer->transfer barrier cannot preserve that older texture version.
    // Split before acquiring the render pass / command buffer or allocating this
    // draw's constants. New images can still be uploaded in the leading buffer.
    if (used_upload_ && REXCVAR_GET(masseffect_native_sort_updates_texture)) {
      for (const Texture* texture : textures_to_upload_) {
        if (!texture->needs_upload || !texture->image.prepared) continue;
        if (++me_ordered_updates_ <= 16 || (me_ordered_updates_ & 255) == 0) {
          REXLOG_INFO("[native] ME texture update ordering: split {} before replacing {:08X} "
                      "{}x{} (command generation {})",
                      me_ordered_updates_, texture->address, texture->image.width,
                      texture->image.height, context_->GenerationCommands());
        }
        if (!context_->SendAndWait()) return false;
        break;
      }
    }
    // --- Upload buffer space ----------------------------------------------------------
    const VkDeviceSize bytes_indices =
        indices_from_16 ? VkDeviceSize(indices16_.size()) * 2 : VkDeviceSize(indices_.size()) * 4;
    // With a single binding the copy goes to a multiple of the stride: up to stride - 4 bytes of padding.
    const VkDeviceSize gap_base_zero =
        vertices_base_zero_ && entry->bindings.size() == 1 ? VkDeviceSize(entry->bindings[0].stride) : 0;
    const VkDeviceSize required = gap_base_zero + bytes_vertices + bytes_indices + bytes_textures +
                                    2 * VkDeviceSize(kRegistersConstants) * 4 +
                                    kSharedWords * 4 + 64 * 8 +
                                    // With UBOs the blocks go whole and aligned to alignment_ubo_
                                    (use_ubo_ ? kUboBytesVs + kUboBytesPs + kUboBytesShared +
                                                     3 * alignment_ubo_
                                               : 0);
    if (required > kUploadSize) {
      return Reject(13, "draw larger than the upload buffer");
    }
    const bool shared_full =
        shared_separate_ && use_ubo_ &&
        shared_used_ + kUboBytesShared + alignment_ubo_ > kSharedSize;
    if (used_upload_ + required > kUploadSize || shared_full) {
      const auto before_send = std::chrono::steady_clock::now();
      const bool sent = context_->SendAndWait();
      ++full_sends_;
      ns_full_sends_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - before_send)
                                        .count());
      if (!sent) {
        return false;
      }
    }

    // --- Render pass ----------------------------------------------------------------
    /*
     * No XXH3 on every draw (masseffect_native_key_fast_pass). If the 40 bytes of render targets are the
     * same ones that produced pass_key_ (BeginPass stores them next to it), their XXH3 is pass_key_.
     * Otherwise it is computed as usual: the pass change decision is the same as before in every case,
     * collisions included.
     */
    uint64_t pass_key;
    if (key_fast_pass_ && pass_valid_keys_ && Equal(keys, pass_keys_, sizeof(keys))) {
      const uint64_t n = ++keys_fast_pass_;
      pass_key = (n <= kKeysPassToCheck || (n & 4095) == 0) ? CheckPassKey(keys, n) : pass_key_;
    } else {
      pass_key = XXH3_64bits(keys, sizeof(keys));
    }
    if (!active_pass_ || pass_key != pass_key_ ||
        pass_generation_ != context_->GenerationCommands()) {
      const auto before_pass = std::chrono::steady_clock::now();
      if (pass_generation_ != context_->GenerationCommands()) {
        ++passes_per_generation_;
      } else if (!active_pass_ && pass_key == pass_key_) {
        ++resumed_passes_;
      } else {
        ++passes_per_target_;
      }
      FinishPass();
      // The pass change is split. FinishPass closes the previous pass; BeginPass finds or creates the
      // render targets (it may create images, record barriers and clear them), builds the render pass and
      // the framebuffer, and opens the pass.
      const auto after_finish = std::chrono::steady_clock::now();
      if (time_) {
        stages_ns_[8] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            after_finish - before_pass).count());
      }
      const bool started = BeginPass(r, keys, pitch, pass_key);
      const auto after_pass = std::chrono::steady_clock::now();
      ns_passes_ += uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
      // The pass change, separate from what the stage costs on each draw.
      if (time_) {
        stages_ns_[7] += uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(after_pass - before_pass).count());
      }
      if (!started) {
        return false;
      }
    }
    const VkCommandBuffer cmd = pass_commands_;
    if (recording_generation_ != context_->GenerationCommands()) {
      recording_generation_ = context_->GenerationCommands();
      pipeline_bound_ = VK_NULL_HANDLE;
      eds_valid_ = false;  // phases 1 and 2: nothing is set in the new buffer
      bound_valid_key_ = false;  // New command buffer (CountChangePipeline)
      sets_bound_ = false;
      ubo_bound_ = false;
      recorded_state_ = false;
      recorded_bindings_ = 0;  // different command buffer: nothing is bound
    }

    Stage(3, mark);
    // --- Uploads: textures, vertices, indices and constants ------------------------------
    for (Texture* texture : textures_to_upload_) {
      if (!UploadTexture(*texture)) {
        return false;
      }
    }
    std::array<VkDeviceSize, 16> offsets_vertices{};
    // If the guest has waited for the GPU since the last draw, it may legally have rewritten an already
    // referenced range: what was recorded is no longer valid.
    if (dedupe_active_) {
      const uint32_t sinc = g_synchronizations_ring.load(std::memory_order_relaxed);
      if (sinc != dedupe_sinc_view_) {
        dedupe_sinc_view_ = sinc;
        dedupe_.Forget();
      }
    }
    const auto before_vertices =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (size_t b = 0; b < entry->bindings.size(); ++b) {
      const Source& source = sources[b];
      if (diag_vertices_repeated_) {
        NoteVerticesRepeated(source.address, source.bytes, uint32_t(source.order), source.data);
      }
      VkDeviceSize offset;
      // Mass Effect's native PM4 frontend does not publish the MASSEFFECT wait
      // generation. Address equality therefore cannot prove that a transient
      // vertex range still contains the copy recorded earlier in this slot.
      // Verify content before reusing it, and expose rejected stale hits.
      // Mass Effect on Switch: the full XXH3 of every range was ~18 % of the ring thread. With
      // masseffect_native_dedupe_sample the check hashes every 8th block (SampleFingerprint) instead.
      static const uint32_t max_fingerprint = uint32_t(REXCVAR_GET(masseffect_native_dedupe_max_fingerprint));
      const uint64_t vertices_fingerprint = dedupe_active_ && type != 8 && (!max_fingerprint || source.bytes <= max_fingerprint)
          ? std::max<uint64_t>(1, REXCVAR_GET(masseffect_native_dedupe_sample)
                                      ? SampleFingerprint(source.data, source.bytes, source.bytes)
                                      : XXH3_64bits(source.data, source.bytes)) : 0;
      const uint64_t discrepancies_before = dedupe_.discrepancies();
      // If this same range was already copied in this frame, its place in the upload buffer is reused and
      // nothing is copied. See masseffect_native_vertices_dedupe.h.
      if (dedupe_active_ && type != 8 &&
          dedupe_.Search(source.address, source.bytes, uint32_t(source.order), offset,
                         vertices_fingerprint)) {
        offsets_vertices[b] = offset;
        continue;
      }
      if (dedupe_.discrepancies() != discrepancies_before && dedupe_.discrepancies() <= 8) {
        REXLOG_WARN("[native] stale vertex reuse prevented: address {:08X}, {} bytes, "
                    "frame {}, discrepancy {}", source.address, source.bytes,
                    frame_, dedupe_.discrepancies());
      }
      // With a single binding, at a multiple of its stride (masseffect_native_vertices_base_zero).
      if (vertices_base_zero_ && entry->bindings.size() == 1) {
        ReserveMultiple(source.bytes, entry->bindings[0].stride, offset);
      } else {
        Reserve(source.bytes, 4, offset);
      }
      const WorkCopy work{source.data, upload_data_ + offset, source.bytes / 4, source.order};
      const uint64_t audit_bytes = uint64_t(work.words) * 4;
      const bool audit_vertex = (type == 8 || !active_copies_) &&
          source.address + audit_bytes <= kPhysicalMemory && StartPayloadAudit(1, audit_bytes);
      const uint64_t audit_before = audit_vertex ? XXH3_64bits(source.data, size_t(audit_bytes)) : 0;
      if (type == 8 || !active_copies_ || !EnqueueCopy(work)) {
        CopyVertices(work);
      }
      if (audit_vertex) {
        // GpuSwap is involutive. Compare the complete staged copy in guest byte
        // order, without modifying the upload allocation or the actual draw.
        std::vector<uint8_t> raw(size_t(audit_bytes), 0);
        for (uint32_t i = 0; i < work.words; ++i) {
          uint32_t word;
          std::memcpy(&word, work.target + size_t(i) * 4, 4);
          word = xenos::GpuSwap(word, work.order);
          std::memcpy(raw.data() + size_t(i) * 4, &word, 4);
        }
        FinalizePayloadAudit(1, source.address, audit_bytes, audit_before,
                              XXH3_64bits(source.data, size_t(audit_bytes)),
                              XXH3_64bits(raw.data(), raw.size()));
      }
      bytes_vertices_ += source.bytes;
      offsets_vertices[b] = offset;
      if (dedupe_active_ && type != 8) {
        dedupe_.Note(source.address, source.bytes, uint32_t(source.order), offset,
                       vertices_fingerprint);
      }
    }
    if (time_) {
      ns_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - before_vertices)
                                   .count());
    }
    VkDeviceSize offset_indices = 0;
    if (with_indices && indices_cache_hit && indices_cache_frame_hit == frame_) {
      offset_indices = indices_cache_offset;  // already uploaded by an earlier draw of this upload buffer
    } else if (with_indices) {
      if (indices_cache_hit) {
        // The upload buffer changed since the lookup (a Reserve forced a submission): convert after all.
        uint32_t mn = 0xFFFF, mx = 0;
        IndicesFrom16(indices_cache_data, uint32_t(indices16_.size()), indices16_.data(), indices_cache_rotate, mn, mx);
        ++indices_cache_lost_;
      }
      Reserve(bytes_indices, 4, offset_indices);
      bytes_indices_uploaded_ += bytes_indices;
      std::memcpy(upload_data_ + offset_indices,
                  indices_from_16 ? static_cast<const void*>(indices16_.data())
                                : static_cast<const void*>(indices_.data()),
                  size_t(bytes_indices));
      if (indices_cache_registrar || indices_cache_hit) {
        IndicesEntryCache& e = indices_cache_[(indices_cache_key * 0x9E3779B97F4A7C15ull >> 40) &
                                                (indices_cache_.size() - 1)];
        e.key = indices_cache_key;
        e.count = uint32_t(indices16_.size());
        e.vmin = vmin;
        e.vmax = vmax;
        e.offset = offset_indices;
        e.fingerprint = indices_cache_fingerprint;
        e.frame = frame_;
        e.generation = indices_cache_generation_;
      }
    }
    // Only the registers each shader reads (ShaderEntry::constants_bytes). With the same generation, the
    // copy is reused if it already covers what this shader needs.
    // Start the bounded audit at the first HDR target so menu cache hits cannot
    // exhaust its coverage before the scene. This only compares host memory.
    for (uint32_t info : {gr::XE_GPU_REG_RB_COLOR_INFO, gr::XE_GPU_REG_RB_COLOR1_INFO,
                          gr::XE_GPU_REG_RB_COLOR2_INFO, gr::XE_GPU_REG_RB_COLOR3_INFO}) {
      const uint32_t format = (r[info] >> 16) & 15;
      if (format == 3 || format == 7 || format == 12) me_constant_audit_hdr_ = true;
    }
    const uint32_t bytes_vs = std::min<uint32_t>(p.vs->constants_bytes, kRegistersConstants * 4);
    if (constants_vs_generation_ != p.generation_constants_vs ||
        constants_vs_epoch_ != epoch_upload_ || bytes_vs > constants_vs_bytes_) {
      // Why they are uploaded again (measurement only; each upload is another set 4 offset).
      ++reuploaded_vs_[constants_vs_generation_ != p.generation_constants_vs ? 0
                      : constants_vs_epoch_ != epoch_upload_                 ? 1
                                                                              : 2];
      // With UBOs the block goes whole (the driver may read the full range) and aligned to the device minimum
      Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_vs, kUboBytesVs) : bytes_vs, use_ubo_ ? alignment_ubo_ : 16,
               constants_vs_offset_);
      std::memcpy(upload_data_ + constants_vs_offset_, r + kRegConstantsVs, bytes_vs);
      constants_vs_generation_ = p.generation_constants_vs;
      constants_vs_epoch_ = epoch_upload_;
      constants_vs_bytes_ = bytes_vs;
    } else {
      AuditCacheConstants(true, p.vs->number, p.generation_constants_vs,
                             r + kRegConstantsVs, constants_vs_offset_, bytes_vs);
    }
    const uint32_t bytes_ps =
        ps ? std::min<uint32_t>(ps->constants_bytes, kRegistersConstants * 4) : 0;
    uint8_t constants_mode_ps = 0;
    uint32_t tonemap_scale_bits = 0;
    float tonemap_scale = 0.0f;
    if (ps && ps->shader->fingerprint == kMeFingerprintTonemap && bytes_ps >= 4) {
      if (REXCVAR_GET(masseffect_native_fix_tonemap_7e3)) {
        constants_mode_ps = 1;
        tonemap_scale = static_cast<float>(
            std::clamp(REXCVAR_GET(masseffect_native_tonemap_7e3_scale), 0.0, 8.0));
        std::memcpy(&tonemap_scale_bits, &tonemap_scale, sizeof(tonemap_scale_bits));
      }
    }
    if (ps && (constants_ps_generation_ != p.generation_constants_ps ||
               constants_ps_epoch_ != epoch_upload_ || bytes_ps > constants_ps_bytes_ ||
               constants_mode_ps != constants_ps_mode_ ||
               tonemap_scale_bits != constants_ps_tonemap_scale_bits_)) {
      ++reuploaded_ps_[constants_ps_generation_ != p.generation_constants_ps ? 0
                      : constants_ps_epoch_ != epoch_upload_                 ? 1
                                                                              : 2];
      Reserve(use_ubo_ ? std::max<VkDeviceSize>(bytes_ps, kUboBytesPs) : bytes_ps, use_ubo_ ? alignment_ubo_ : 16,
               constants_ps_offset_);
      std::memcpy(upload_data_ + constants_ps_offset_, r + kRegConstantsPs, bytes_ps);
      if (constants_mode_ps) {
        // UE3 writes this composition with SCENE_COLOR_BIAS_FACTOR=8 for Xenos' packed 7e3
        // output path. In the native renderer the EDRAM view is canonical FP16, so carrying that
        // packed-domain scale to the host attachment saturates the later UNORM presentation. The
        // measured equivalent at this boundary is 8 / 256 = 1 / 32.
        std::memcpy(upload_data_ + constants_ps_offset_, &tonemap_scale, sizeof(tonemap_scale));
      }
      constants_ps_generation_ = p.generation_constants_ps;
      constants_ps_epoch_ = epoch_upload_;
      constants_ps_bytes_ = bytes_ps;
      constants_ps_mode_ = constants_mode_ps;
      constants_ps_tonemap_scale_bits_ = tonemap_scale_bits;
    } else if (ps && !constants_mode_ps) {
      // The tonemap override intentionally differs from the guest bank.
      AuditCacheConstants(false, ps->number, p.generation_constants_ps,
                             r + kRegConstantsPs, constants_ps_offset_, bytes_ps);
    }

    Stage(4, mark);
    // --- Viewport, scissor and shared constants ---------------------------------------
    /*
     * The framing (viewport, ndc and scissor), cached (masseffect_native_framing_cache).
     *
     * The viewport, the ndc and the scissor are a function of the framing registers (PA_CL_VTE_CNTL,
     * PA_CL_VPORT_*, PA_SC_WINDOW_OFFSET/SCISSOR, PA_CL_CLIP_CNTL and PA_SU_SC_MODE_CNTL:
     * IsRegisterOfFraming in the native graphics system, which bumps generation_framing on every path that
     * writes them) and of the pass size and scale (pass_series_). In play the framing really changes
     * 0.07-0.14 times per draw ("C6 generations"). The depth bias is not included: it depends on
     * PA_SU_POLY_OFFSET_*, RB_DEPTH_INFO and the primitive type, which do not bump that generation.
     */
    const uint32_t vte = r[gr::XE_GPU_REG_PA_CL_VTE_CNTL];
    const auto phase_probe = me::native::GetMsaa2PhaseProbe(
        uint32_t(REXCVAR_GET(masseffect_native_diag_msaa2_phase)),
        (r[gr::XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3u, no_clip, vte);
    if (!phase_probe.valid) return Reject(53, "unsupported collapsed-2x phase diagnostic raster contract");
    if (phase_probe.active && (pass_scale_ != 1.0f || pass_raster_scale_x_ != 1.0f))
      return Reject(53, "collapsed-2x phase diagnostic requires unscaled raster pixels");
    if (pass_raster_scale_x_ != 1.0f && pass_scale_ != 1.0f)
      return Reject(53, "guest FragCoord XY grid remapping does not support shadow resolution scaling");
    const uint32_t phase_probe_identity = phase_probe.active ?
        uint32_t(REXCVAR_GET(masseffect_native_diag_msaa2_phase)) : 0;
    VkViewport viewport{};
    float ndc[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    VkRect2D scissor{};
    uint32_t empty_framing = 0;
    const bool framing_in_cache = framing_cache_ && valid_framing_ &&
                                   p.generation_framing == framing_generation_ && pass_series_ == framing_pass_series_ &&
                                   framing_depth_half_ == pass_depth_float24_half_ &&
                                   framing_phase_probe_ == phase_probe_identity;
    bool check_framing = false;
    if (framing_in_cache) {
      const uint64_t n = ++framing_hits_;
      check_framing = n <= kFramingsToCheck || (n & 4095) == 0;
      ++framing_hits_report_;
    }
    if (framing_in_cache && !check_framing) {
      viewport = framing_viewport_;
      std::memcpy(ndc, framing_ndc_, sizeof(ndc));
      scissor = framing_scissor_;
      empty_framing = empty_framing_;
    } else {
      empty_framing = ComputeFraming(r, vte, no_clip, sc_mode, viewport, ndc, scissor, phase_probe);
      ++framing_computations_report_;
      if (check_framing) {
        CompareFraming(viewport, ndc, scissor, empty_framing);  // with the stored values, before overwriting them
      }
      framing_viewport_ = viewport;
      std::memcpy(framing_ndc_, ndc, sizeof(ndc));
      framing_scissor_ = scissor;
      empty_framing_ = empty_framing;
      framing_generation_ = p.generation_framing;
      framing_pass_series_ = pass_series_;
      framing_depth_half_ = pass_depth_float24_half_;
      framing_phase_probe_ = phase_probe_identity;
      valid_framing_ = true;
    }
    if (empty_framing != 0) {
      return true;  // empty viewport or scissor: as before
    }

    uint32_t specialization = entry->specialization;
    if (pass_depth_float24_half_) specialization |= kSpecDepthFloat24Half;
    const int float24_mode = REXCVAR_GET(masseffect_native_float24_ps_mode);
    if (float24_mode < 0 || float24_mode > 2)
      return Reject(51, "FLOAT24 quantization mode must be 0 (off), 1 (truncate), or 2 (round)");
    if (pass_depth_float24_half_ && float24_mode && (r[gr::XE_GPU_REG_RB_DEPTHCONTROL] & 2)) {
      // Final depth clipping must not undo quantization at an unrepresentable
      // viewport endpoint. Reject that unsupported experiment explicitly.
      if (me::native::QuantizeDepthHalfCpu(viewport.minDepth, float24_mode == 2) != viewport.minDepth ||
          me::native::QuantizeDepthHalfCpu(viewport.maxDepth, float24_mode == 2) != viewport.maxDepth)
        return Reject(51, "FLOAT24 quantization needs exactly representable viewport depth bounds");
      specialization |= kSpecDepthFloat24Quantize;
      if (float24_mode == 2) specialization |= kSpecDepthFloat24Round;
    }
    if (pass_raster_scale_x_ != 1.0f) specialization |= kSpecRasterGridX;
    if (type == 8) specialization |= kSpecRectangle;
    const uint32_t control_color = r[gr::XE_GPU_REG_RB_COLORCONTROL];
    float threshold_alpha = 0.0f;
    uint32_t function_alpha = 7;  // always
    // The 8 Xenos functions (0 never, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 always) with alphaTestValue
    // (masseffect_validated_normal library). Without a PS (mode 5) there is no test.
    if (ps && ((control_color >> 3) & 0x1) && (control_color & 0x7) != 7) {
      threshold_alpha = Float(r[gr::XE_GPU_REG_RB_ALPHA_REF]);
      function_alpha = control_color & 0x7;
      specialization |= 0x2;
      // The function goes in the pipeline, not in the constants. RB_COLORCONTROL was already part of the key,
      // so this creates no pipelines that did not already exist.
      specialization |= (function_alpha & 0x7u) << kSpecFunctionAlphaDisplacement;
    }
    shared[64] = (r[kRegBooleans] & 0xFFFF) | ((r[kRegBooleans + 4] & 0xFFFF) << 16);
    shared[65] = 0;  // g_SwappedTexcoords
    if (!(r[gr::XE_GPU_REG_PA_SU_VTX_CNTL] & 0x1)) {  // PixelCenter::kD3DZero
      // D3D's half pixel is a GUEST half pixel. One guest pixel spans the
      // expanded X grid, whereas the viewport extent below is in HOST pixels.
      const float mid[2] = {pass_raster_scale_x_ / viewport.width,
                              -1.0f / std::abs(viewport.height)};
      std::memcpy(&shared[66], mid, sizeof(mid));
    }
    std::memcpy(&shared[68], &threshold_alpha, sizeof(threshold_alpha));
    shared[69] = function_alpha;  // g_AlphaFunction
    std::memcpy(&shared[70], ndc, sizeof(ndc));
    std::copy(entry->remaps.begin(), entry->remaps.end(), shared + 74);
    VkDeviceSize offset_shared;
    /*
     * How many draws really change the shared constants.
     *
     * This block is 488 bytes that are zeroed, filled, compared and, if they changed, copied twice, once
     * into memory without CPU caching (2654 MB/s). With ~2,400 draws per frame that is ~4.7 MB of traffic
     * per frame on the thread that is already at 96 % of a core.
     *
     * Which fix applies depends on the count: if almost no draw changes the block, the comparison is the
     * waste; if almost all do, the double memcpy is. They are two different fixes, and counting costs one
     * increment.
     */
    ++shared_looked_;
    const auto t_shared =
        time_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (shared_epoch_ == epoch_upload_ &&
        Equal(shared, shared_previous_, sizeof(shared))) {
      offset_shared = shared_offset_;  // the same as the previous draw
    } else {
      ++shared_changed_;
      if (shared_separate_ && use_ubo_) {
        // To the separate CPU-cached buffer; binding 2 of set 4 points to it.
        offset_shared = (shared_used_ + alignment_ubo_ - 1) & ~(alignment_ubo_ - 1);
        shared_used_ = offset_shared + kUboBytesShared;
        std::memcpy(shared_data_ + offset_shared, shared, sizeof(shared));
      } else {
        Reserve(use_ubo_ ? kUboBytesShared : kSharedWords * 4, use_ubo_ ? alignment_ubo_ : 16,
                 offset_shared);
        std::memcpy(upload_data_ + offset_shared, shared, sizeof(shared));
      }
      std::memcpy(shared_previous_, shared, sizeof(shared));
      shared_offset_ = offset_shared;
      shared_epoch_ = epoch_upload_;
    }
    if (time_) {
      sub_ns_[12] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - t_shared)
                                  .count());
      ++sub_n_[12];
    }

    // --- Pipeline ---------------------------------------------------------------------------
    PipelineKey key;
    key.vs = p.vs->number + 1;
    key.ps = ps ? ps->number + 1 : 0;  // 0: mode 5, no fragment stage
    key.entry = entry->fingerprint;
    key.topology = uint32_t(topology);
    if (use_ubo_) {
      specialization |= kSpecConstantsUbo;  // the shaders read from set 4
    }
    if (inv_tex_size_) {
      specialization |= kSpecInvTexSize;
    }
    if (pcf_cheap_) {
      specialization |= kSpecPcfCheap;
    }
    if (alpha_only && ps) {
      // If vegetation is excluded from the shadow map, this whole draw is unnecessary.
      if (no_vegetation_) {
        return true;
      }
      specialization |= kSpecAlphaOnly;
    }
    /*
     * Test depth before shading where it costs nothing.
     *
     * A pixel shader that can discard (alpha test or its own kill) forces the hardware to shade first and
     * test Z afterwards: otherwise a discarded fragment would already have written its depth. But that only
     * matters if the draw writes depth. If it only tests it (smoke, particles, glass, decals, lights),
     * testing earlier is exact: there is no write to move, the shader still discards the color, and the GPU
     * stops shading what is hidden.
     *
     * The counters are kept even with the setting off, so the report says the same in both halves of an A/B
     * test, and the "CANNOT: they write depth" line measures exactly what a depth pre-pass
     * would have to fix.
     */
    if (ps && keys[4] && masks) {
      const bool test_z = ((control_depth >> 1) & 0x1) != 0;
      const bool writes_z = test_z && ((control_depth >> 2) & 0x1) != 0;
      const bool stencil = (control_depth & 0x1) != 0;
      const bool ps_writes_z = (ps->outputs & 0x10) != 0;
      const bool late = (specialization & 0x2) || ps->discards || ps_writes_z;
      if (!test_z) {
        // Without a depth test there is nothing to move earlier.
      } else if (!late) {
        ++counts_z_[kZAlreadyEarly];
      } else if (writes_z || ps_writes_z) {
        ++counts_z_[kZWritesZ];
      } else if (stencil) {
        ++counts_z_[kZStencil];
      } else if (ps->shader) {
        ++counts_z_[kZSet];
        if (z_early_) {
          specialization |= kSpecZEarly;
        }
      }
    }
    uint32_t outputs_mask_7e3 = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!keys[i]) continue;
      const uint32_t format = (r[kInfoColor[i]] >> 16) & 0xF;
      const int32_t bias_color = int32_t(r[kInfoColor[i]] << 6) >> 26;
      if (bias_color && biases_color_recorded_.insert(r[kInfoColor[i]]).second) {
        REXLOG_WARN("[native] ME output-merger RT{} info {:08X}: format {} exponent bias {} (factor {})",
                    i, r[kInfoColor[i]], format, bias_color, std::ldexp(1.0f, bias_color));
      }
      if (format == uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT) ||
          format == uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
        outputs_mask_7e3 |= 1u << i;
      }
    }
    if (outputs_mask_7e3) {
      specialization |= kSpecTarget7e3 |
                          (outputs_mask_7e3 << kSpecMask7e3Displacement);
    }
    key.specialization = specialization;
    std::copy(std::begin(pass_formats_), std::end(pass_formats_), std::begin(key.formats));
    static constexpr uint32_t kBlend[4] = {
        gr::XE_GPU_REG_RB_BLENDCONTROL0, gr::XE_GPU_REG_RB_BLENDCONTROL1,
        gr::XE_GPU_REG_RB_BLENDCONTROL2, gr::XE_GPU_REG_RB_BLENDCONTROL3};
    for (uint32_t i = 0; i < 4; ++i) {
      if (keys[i]) {
        key.blend[i] = r[kBlend[i]] & 0x1FFF1FFF;
      }
    }
    key.masks = masks;
    key.depth = keys[4] ? control_depth : 0;
    key.rasterization = (sc_mode & 0x7) | (reset && !fan ? 0x8 : 0) | (with_bias ? 0x10 : 0);
    if (phase_probe.active) {
      key.rasterization |= me::native::kNativeMsaa2PhaseProbe;
      if (phase_probe_identity == 2) key.rasterization |= me::native::kNativeMsaa2PhaseMode2;
    }
    // PA_CL_CLIP_CNTL.clip_disable must also disable host Z clipping. ME1's
    // physical EDRAM clear rectangles deliberately use a slightly negative Z;
    // clipping them leaves previous-frame color/stencil words behind.
    if (me::native::XenosDepthClamp(r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL], vulkan_device_->properties().depthClamp))
      key.rasterization |= me::native::kNativeDepthClamp;
    // Same as rexglue draw_util::IsPrimitivePolygonal: rectangle lists have
    // no front/back culling even though the host expansion uses triangles.
    if (type == 8) key.rasterization &= ~0x3u;
    VkPipeline pipeline = VK_NULL_HANDLE;
    // Looked up with SearchKey (phase 0a: canonical form; phases 1 and 2: without the state set through
    // vkCmdSet*). the key stays raw: the deferred sky (opaque_in_all), the counter and the dynamic state read
    // it.
    MASSEFFECT_SUB(11, pipeline = PipelineFor(SearchKey(key), *entry, p));
    if (pipeline == VK_NULL_HANDLE) {
      return false;
    }
    // The pipeline about to be bound, prefetched into the cache several us before its vkCmdBindPipeline
    // (PRFM only: it reads and writes nothing and cannot fail).
    if (nvk_preload_app_ && pipeline != pipeline_bound_ && vk_switch_preload_pipeline) {
      vk_switch_preload_pipeline(pipeline);
    }

    Stage(5, mark);
    /*
     * The three dynamic state values that used to be computed inside the recording block are computed here,
     * without recording anything. They are pure computations on the draw's registers; they are needed
     * earlier so they can be saved if this draw is the sky and gets deferred (see below).
     */
    /*
     * Record how far down the game draws in this render target. It only grows, so the pass is never opened
     * smaller than what has already been seen drawn.
     */
    if (area_util_) {
      const uint32_t until = uint32_t(std::max(0, scissor.offset.y + int32_t(scissor.extent.height)));
      const uint32_t rounded = (until + 63u) & ~63u;
      uint32_t& pointed = UtilHeightFor(pitch);  // without a map lookup on every draw
      if (rounded > pointed) {
        pointed = rounded;
      }
    }
    const float blend_constant[4] = {Float(r[gr::XE_GPU_REG_RB_BLEND_RED]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_GREEN]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_BLUE]),
                                       Float(r[gr::XE_GPU_REG_RB_BLEND_ALPHA])};
    const uint32_t stencil_front = r[gr::XE_GPU_REG_RB_STENCILREFMASK];
    const uint32_t stencil_back = ((control_depth >> 7) & 0x1)
                                       ? r[gr::XE_GPU_REG_RB_STENCILREFMASK_BF]
                                       : stencil_front;
    /*
     * The sky, deferred until after the opaque draws (masseffect_native_postponed_sky).
     *
     * How it is recognized, and why it cannot be mistaken for another draw. Three conditions at once, none of
     * them a magic position or draw count:
     *
     *  1. The pixel shader is the sky's, by the fingerprint of its original container (kSkyFingerprint). It is
     *     the only one of the library's 152 shaders whose constant table names CloudIntensity and
     *     SkyAlphaTag. The fingerprint does not depend on the SPIR-V translation or on the library order.
     *  2. The draw writes color and is opaque on all its targets: blending is "1 x source + 0 x destination"
     *     (ADD) on the channels the mask writes, exactly the criterion PipelineFor uses to decide
     *     blendEnable. Opaque = it does not read the existing color, so whatever was painted before it does
     *     not matter.
     *  3. It tests depth and does not write it, and there is no stencil. Without a Z write, deferring it
     *     cannot change what later draws see; with the test on, in its new place the Z test discards it
     *     where the world used to overwrite it.
     *
     * If any of the three fails (another version of the game, another state) the draw takes the usual path
     * and nothing happens.
     */
    // A draw with no color to write does not read the target: it does not count as transparent.
    bool opaque_in_all = true;
    for (uint32_t i = 0; i < 4 && opaque_in_all; ++i) {
      const uint32_t i_mask = (masks >> (i * 4)) & 0xF;
      if (!i_mask) {
        continue;
      }
      const uint32_t m = key.blend[i];
      const bool color_direct = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool direct_alpha =
          ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      const bool on_blend =
          (((i_mask & 0x7) != 0) && !color_direct) || (((i_mask & 0x8) != 0) && !direct_alpha);
      if (on_blend) {
        opaque_in_all = false;
      }
    }
    const bool test_z_draw = ((control_depth >> 1) & 0x1) != 0;
    const bool writes_z_draw = test_z_draw && ((control_depth >> 2) & 0x1) != 0 &&
                                  keys[4] != 0;
    bool is_sky = false;
    if (ps && ps->shader && ps->shader->fingerprint == kSkyFingerprint && masks) {
      ++seen_sky_;
      const bool stencil = (control_depth & 0x1) != 0;
      /*
       * Scene pass only. The same dome is also drawn in cubemaps and reflections, and there it
       * is not touched, and the measured saving (4.8 of the scene's 19.2 ms) is the scene's. This line is
       * what keeps those out of it.
       */
      const bool in_the_scene = CategoryOfTarget(pitch, keys) == kGpuScene;
      /*
       * The fingerprint is not enough, and that broke an earlier version.
       *
       * The fingerprint identifies the shader, not the draw. The counter said so: "7.00 detected per
       * frame" when the analysis expected 0.9. Seven draws use that pixel shader, and deferring one broke
       * the order of the other six: flickering and odd colors.
       *
       * Now it must also be the dome:
       *   - the index count measured by the analysis of the SPIR-V and the log: 480 = 160 triangles;
       *   - and it must be the first draw of the pass (the dome is painted on the empty Z-buffer, which is
       *     exactly why it shades the whole screen and is then covered).
       *
       * Both are cheap to check and both belong to the dome, not to the shader. If the counter still said
       * more than one per frame, nothing is deferred: there is a guard below.
       */
      const uint32_t indices_draw =
          uint32_t(indices_from_16 ? indices16_.size() : indices_.size());
      const bool geometry_of_dome = indices_draw == kIndicesDomeSky;
      const bool first_of_the_pass = draws_in_pass_ == 0;
      /* The full criterion, without the cvar or the guard: what is measured during the test. */
      const bool is_the_dome = in_the_scene && opaque_in_all && test_z_draw &&
                              !writes_z_draw && !stencil && !(ps->outputs & 0x10) &&
                              geometry_of_dome && first_of_the_pass;
      if (is_the_dome) {
        ++sky_candidates_;  // the ones matching the dome's marks: this must be 1
        /*
         * The guard counts here, whether it defers or not. Two in the same frame is exactly what broke the
         * image before, so as soon as it happens it switches off and stays off for the whole session.
         */
        if (++sky_guard_in_frame_ > 1 && sky_guard_ != kDiscardedSky) {
          sky_guard_ = kDiscardedSky;
          REXLOG_WARN("[native] C6 sky: the guard sees {} domes in the SAME frame; nothing is deferred "
                      "for the whole session (build 137 failure). The image is left untouched",
                      sky_guard_in_frame_);
        }
      }
      is_sky = postponed_sky_ && is_the_dome && sky_guard_ == kSkyPostponing;
      if (!is_sky && postponed_sky_) {
        ++sky_no_postponable_;  // only with the setting on: measures criterion failures, not the cvar
      }
    }
    /*
     * Which draws the sky can be moved past, and why that does not change a single pixel.
     *
     * The sky can only skip past a draw D if both hold:
     *
     *  - D is opaque: it does not read the existing color, so wherever D paints, the final color is its own
     *    whether or not the sky was underneath. A draw with blending does read the background: the sky goes
     *    first.
     *  - D writes depth: after D the Z-buffer holds D's z, which is closer than the dome, so the sky's LEQUAL
     *    test discards it exactly where D painted. If D did not write Z, the Z-buffer would stay as it was
     *    and the sky would be painted over D: that would change the image.
     *
     * So the sky is emitted as soon as the first draw that fails either condition arrives. In practice that
     * is the first transparent draw, because what follows the sky is the opaque world; the counter separates
     * the two reasons so this can be checked in the log.
     */
    if (pending_sky_) {
      if (is_sky) {
        ++sky_two_in_pass_;  // another sky in the same pass: emit the earlier one now, keeping the order
        EmitPostponedSky(cmd, kSkyPerOtherSky);
      } else if (!(opaque_in_all && writes_z_draw)) {
        EmitPostponedSky(cmd, opaque_in_all ? kSkyPerNoZ : kSkyPerBlend);
      }
    }
    // --- Record ---------------------------------------------------------------------------------
    if (is_sky) {
      // The arguments of the vkCmd* calls this draw would have emitted are saved; nothing is recorded. The
      // upload buffer is an allocator that only moves forward (Reserve) and is not reset until UseSlot,
      // which always comes after BeforeSend -> FinishPass: when the sky is emitted, its offsets still
      // point to the same data.
      PostponedSky& c = sky_;
      c.pipeline = pipeline;
      c.buffer = upload_;
      c.usa_ubo = use_ubo_;
      c.push[0] = upload_address_ + constants_vs_offset_;
      c.push[1] = upload_address_ + constants_ps_offset_;
      c.push[2] = upload_address_ + offset_shared;
      c.offsets_ubo = use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constants_vs_offset_),
                                                          uint32_t(constants_ps_offset_),
                                                          uint32_t(offset_shared)}
                                : std::array<uint32_t, 3>{0, 0, 0};
      c.slot_ubo = slot_current_;
      c.viewport = viewport;
      c.scissor = scissor;
      std::memcpy(c.blend, blend_constant, sizeof(c.blend));
      std::memcpy(c.bias, bias, sizeof(c.bias));
      c.with_stencil = keys[4] != 0;
      c.stencil[0] = stencil_front;
      c.stencil[1] = stencil_back;
      c.n_bindings = uint32_t(entry->bindings.size());
      c.offsets_vertices = offsets_vertices;
      c.with_indices = with_indices;
      c.indices_from_16 = indices_from_16;
      c.indices = uint32_t(indices_from_16 ? indices16_.size() : indices_.size());
      c.first_index = uint32_t(offset_indices / (indices_from_16 ? 2 : 4));
      c.vmin = vmin;
      c.count = count;
      c.ps_more_one = ps ? ps->number + 1 : 0;
      c.vs_more_one = p.vs ? p.vs->number + 1 : 0;
      c.category = CategoryOfTarget(pitch, keys);
      c.draws_on_postpone = draws_in_pass_;
      c.eds_mode = eds_mode_;  // phases 1 and 2: its pipeline lacks this state, so save it all
      if (eds_mode_) {
        EdsStateFor(key, c.eds, eds_mode_);
      }
      pending_sky_ = true;
      ++postponed_sky_count_;
      // Counted as if it had been recorded: it will be recorded before the pass closes.
      ++draws_per_category_[c.category];
      triangles_per_category_[c.category] += (with_indices ? c.indices : count) / 3;
      Stage(6, mark);
      ++drawn_;
      // draws_in_pass_ is not incremented here: this draw is not recorded yet. It is incremented in
      // EmitPostponedSky, and the difference with draws_on_postpone gives the draws that went ahead of
      // it, which is exactly the geometry that now covers it.
      drawn_timed_ += time_ ? 1 : 0;
      return true;
    }
    context_->CountDrawMarkGpu();
    if (label_pass_) {
      label_pass_ = false;
      context_->LabelMarkGpu((uint32_t(p.vs ? p.vs->number : 0xFFFF) << 16) | (p.ps ? p.ps->number & 0xFFFF : 0xFFFF));
    }
    if (pipeline != pipeline_bound_) {
      if (count_changes_pipeline_) {  // Measurement only (masseffect_native_count_changes_pipeline)
        CountChangePipeline(key, pipeline_bound_ == VK_NULL_HANDLE);
      }
      MASSEFFECT_SUB(0, dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
      pipeline_bound_ = pipeline;
    }
    // Phases 1 and 2. The state that no longer goes in the pipeline, before the draw. If the guard sees a
    // difference, dynamic state switches off and this same draw is bound with its usual pipeline.
    if (eds_mode_ && !PinDynamicState(cmd, key)) {
      MASSEFFECT_SUB(11, pipeline = PipelineFor(SearchKey(key), *entry, p));
      if (pipeline == VK_NULL_HANDLE) {
        return false;
      }
      MASSEFFECT_SUB(0, dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline));
      pipeline_bound_ = pipeline;
      bound_valid_key_ = false;  // the counter does not classify this bind
    }
    if (!sets_bound_) {
      MASSEFFECT_SUB(1, dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 4,
                                                sets_.data(), 0, nullptr));
      sets_bound_ = true;
    }
    // Dynamic state and push constants repeat a lot between consecutive draws: they are only recorded if
    // they change (recording: 0.4 us per draw in play).
    const bool recorded = recorded_state_;
    // The SPIR-V statically references both branches of the specialization
    // constant. Without maintenance4, UBO specialization does not waive push
    // constant initialization (VUID-vkCmdDraw-None-08601). Keep both paths valid.
    {
      const uint64_t push[3] = {upload_address_ + constants_vs_offset_,
                                upload_address_ + constants_ps_offset_,
                                upload_address_ + offset_shared};
      if (!recorded || !Equal(push, push_recorded_, sizeof(push))) {
        dfn_.vkCmdPushConstants(cmd, layout_pipeline_,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(push), push);
        std::memcpy(push_recorded_, push, sizeof(push));
      }
    }
    // Set 4 (dynamic UBOs). Always bound, because the new shaders use it statically: with the real offsets
    // if masseffect_native_constants_ubo is set, and 0 otherwise. Only recorded when something changes.
    {
      const std::array<uint32_t, 3> offsets_ubo =
          use_ubo_ ? std::array<uint32_t, 3>{uint32_t(constants_vs_offset_), uint32_t(constants_ps_offset_),
                                              uint32_t(offset_shared)}
                    : std::array<uint32_t, 3>{0, 0, 0};
      ++set4_draws_;  // C6 set 4 report
      if (!ubo_bound_ || slot_ubo_bound_ != slot_current_ || offsets_ubo != offsets_ubo_bound_) {
        // Which offsets change at each bind (measurement only). What NVK saves by differences depends on it:
        // every cbuf whose offset does not change is one rebind less in the Draw.
        if (!ubo_bound_ || slot_ubo_bound_ != slot_current_) {
          ++set4_first_;
        } else {
          ++set4_changes_[(offsets_ubo[0] != offsets_ubo_bound_[0] ? 1u : 0u) |
                          (offsets_ubo[1] != offsets_ubo_bound_[1] ? 2u : 0u) |
                          (offsets_ubo[2] != offsets_ubo_bound_[2] ? 4u : 0u)];
        }
        MASSEFFECT_SUB(2, dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 4, 1,
                                                  &sets_ubo_[slot_current_], 3, offsets_ubo.data()));
        offsets_ubo_bound_ = offsets_ubo;
        slot_ubo_bound_ = slot_current_;
        ubo_bound_ = true;
      }
    }
    if (!recorded || !Equal(&viewport, &viewport_recorded_, sizeof(viewport))) {
      MASSEFFECT_SUB(3, dfn_.vkCmdSetViewport(cmd, 0, 1, &viewport));
      viewport_recorded_ = viewport;
    }
    if (!recorded || !Equal(&scissor, &recorded_scissor_, sizeof(scissor))) {
      MASSEFFECT_SUB(4, dfn_.vkCmdSetScissor(cmd, 0, 1, &scissor));
      recorded_scissor_ = scissor;
    }
    if (!recorded ||
        !Equal(blend_constant, recorded_blend_, sizeof(blend_constant))) {
      MASSEFFECT_SUB(5, dfn_.vkCmdSetBlendConstants(cmd, blend_constant));
      std::memcpy(recorded_blend_, blend_constant, sizeof(blend_constant));
    }
    // Also without bias: the state is dynamic in every pipeline and has to be recorded.
    if (!recorded || !Equal(bias, recorded_bias_, sizeof(bias))) {
      MASSEFFECT_SUB(6, dfn_.vkCmdSetDepthBias(cmd, bias[0], 0.0f, bias[1]));
      std::memcpy(recorded_bias_, bias, sizeof(bias));
    }
    if (!recorded) {
      stencil_recorded_valid_ = false;
      indices_type_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
    }
    if (keys[4]) {
      const uint32_t front = stencil_front;  // computed before the recording block
      const uint32_t back = stencil_back;
      if (!stencil_recorded_valid_ || front != stencil_recorded_[0] ||
          back != stencil_recorded_[1]) {
        MASSEFFECT_SUB(7, {
          dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, front & 0xFF);
          dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, back & 0xFF);
          dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (front >> 8) & 0xFF);
          dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (back >> 8) & 0xFF);
          dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (front >> 16) & 0xFF);
          dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (back >> 16) & 0xFF);
        });
        stencil_recorded_[0] = front;
        stencil_recorded_[1] = back;
        stencil_recorded_valid_ = true;
      }
    }
    recorded_state_ = true;
    /*
     * masseffect_native_vertices_base_zero. With a single binding whose copy starts at a multiple of its stride,
     * the upload buffer is bound at 0 (once per command buffer, or after the deferred sky) and the draw is
     * shifted with first_vertex. The others bind as usual.
     */
    bool base_zero = false;
    uint32_t first_vertex = 0;  // offset / stride: where the copy starts, in vertices
    if (vertices_base_zero_ && type != 8 && entry->bindings.size() == 1) {
      const VkDeviceSize stride = entry->bindings[0].stride;
      if (offsets_vertices[0] % stride == 0) {
        base_zero = CheckBaseZero(offsets_vertices[0], stride, sources[0].bytes, vmin, first_vertex);
      } else {
        ++base_misaligned_zero_;
      }
    } else if (entry->bindings.size() > 1) {
      ++base_zero_several_bindings_;
    }
    if (!entry->bindings.empty()) {
      // 16 identical pointers were filled in on every draw to bind one or two. It is filled when the upload
      // buffer changes (once per work slot), not 2,345 times per frame.
      if (buffers_vertices_[0] != upload_) {
        buffers_vertices_.fill(upload_);
        recorded_bindings_ = 0;  // the buffer changed: earlier bindings are stale
      }
      /*
       * And do not bind again if it is exactly what is already bound.
       *
       * In NVK each binding is 5 dwords plus an invocation of the MME macro NVK_MME_BIND_VB
       * (nvk_cmd_draw.c:4658), with no redundancy check inside: ~2,800 MME macros per frame. With vertex
       * deduplication 28-33 % of the bindings reuse the same offset, so two consecutive draws of the same
       * mesh give exactly the same offsets.
       */
      // On the base-zero path what gets bound is the whole buffer, from 0.
      static constexpr VkDeviceSize kBindingInZero = 0;
      const VkDeviceSize* a_bind = base_zero ? &kBindingInZero : offsets_vertices.data();
      const uint32_t n_bindings = uint32_t(entry->bindings.size());
      bool equal = n_bindings == recorded_bindings_;
      for (uint32_t i = 0; equal && i < n_bindings; ++i) {
        equal = a_bind[i] == offsets_recorded_[i];
      }
      if (!equal) {
        MASSEFFECT_SUB(8, dfn_.vkCmdBindVertexBuffers(cmd, 0, n_bindings, buffers_vertices_.data(), a_bind));
        ++base_zero_recorded_bindings_;
        recorded_bindings_ = n_bindings;
        for (uint32_t i = 0; i < n_bindings; ++i) {
          offsets_recorded_[i] = a_bind[i];
        }
      }
      ++base_zero_total_;
      base_zero_draws_ += base_zero ? 1 : 0;
    }
    // Draws and triangles per render target type (same classification as the GPU time).
    {
      const uint32_t category = CategoryOfTarget(pitch, keys);
      const uint32_t vertices_draw = with_indices ? uint32_t(indices_from_16 ? indices16_.size() : indices_.size())
                                                   : count;
      ++draws_per_category_[category];
      triangles_per_category_[category] += vertices_draw / 3;
    }
    // In the diagnostic frame, one query per draw with its pixel shader.
    // The diagnostic window is decided by the context, at the Swap. With the diagnostic off (the normal
    // case) this does not even reach the call.
    const uint32_t query_draw =
        !diag_stats_draw_
            ? UINT32_MAX
            : context_->BeginStatsDraw(
                  ps ? ps->number + 1 : 0,
                  CategoryOfTarget(pitch, keys), p.vs ? p.vs->number + 1 : 0);
    if (label_gpu_) {
      const auto name = fmt::format(
          "ME frame {} draw {} VS n{}/{:016X} PS n{}/{:016X} pitch {} RT0 {:03X}/{} mask {:04X}",
          frame_, drawn_ + 1, p.vs->number, p.vs->fingerprint,
          ps ? int32_t(ps->number) : -1, ps ? ps->fingerprint : uint64_t(0), pitch,
          uint32_t(keys[0] >> 24) & 0xFFF, uint32_t(keys[0] >> 16) & 0xF,
          r[gr::XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF);
      VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
      label.pLabelName = name.c_str();
      label_gpu_(cmd, &label);
    }
    if (with_indices) {
      // The upload buffer is bound once per index type and each draw uses firstIndex.
      const VkIndexType indices_type = indices_from_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      if (indices_type != indices_type_recorded_) {
        MASSEFFECT_SUB(9, dfn_.vkCmdBindIndexBuffer(cmd, upload_, 0, indices_type));
        indices_type_recorded_ = indices_type;
      }
      // On the base-zero path, vertexOffset also carries where the copy starts (in vertices).
      const int32_t vertices_displacement =
          base_zero ? int32_t(int64_t(first_vertex) - int64_t(vmin)) : -int32_t(vmin);
      const uint32_t indices_draw = uint32_t(indices_from_16 ? indices16_.size() : indices_.size());
      MASSEFFECT_SUB(10, dfn_.vkCmdDrawIndexed(cmd, indices_draw,
                                          1, uint32_t(offset_indices / (indices_from_16 ? 2 : 4)),
                                          vertices_displacement, 0));
    } else {
      MASSEFFECT_SUB(10, dfn_.vkCmdDraw(cmd, count, 1,
                                   base_zero ? first_vertex : 0, 0));  // firstVertex
    }
    if (query_draw != UINT32_MAX) {
      context_->FinishStatsDraw(query_draw);
    }
    Stage(6, mark);
    if (time_) {
      ++sub_samples_;
      ReportSubstages();
    }
    ++drawn_;
    ++draws_in_pass_;  // to know at which position of the pass the sky ends up emitted
    drawn_timed_ += time_ ? 1 : 0;
    return true;
  }

  /*
   * Emits the deferred sky draw (masseffect_native_postponed_sky).
   *
   * It records exactly the same vkCmd* calls it would have recorded in its original place, with the
   * values saved then. It relies on nothing recorded afterwards: it sends all of this draw's dynamic
   * state, and when done it invalidates the tracked state so the next draw records its own again. That
   * way the order cannot slip through an "already set" comparison.
   *
   * The upload buffer offsets are still valid because Reserve only moves forward and the buffer is not
   * reset until UseSlot, which always comes after BeforeSend -> FinishPass.
   */
  void EmitPostponedSky(VkCommandBuffer cmd, uint32_t reason) {
    if (!pending_sky_ || cmd == VK_NULL_HANDLE) {
      return;
    }
    pending_sky_ = false;
    const PostponedSky& c = sky_;
    ++emitted_sky_[reason < kSkyReasons ? reason : kSkyReasons - 1];
    const uint64_t position = draws_in_pass_ - c.draws_on_postpone;
    sky_position_sum_ += position;
    sky_position_max_ = std::max(sky_position_max_, position);
    dfn_.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipeline);
    pipeline_bound_ = c.pipeline;
    if (c.eds_mode) {  // phases 1 and 2: all the state its pipeline lacks
      EmitDynamicState(cmd, c.eds, true, c.eds_mode);
    }
    eds_valid_ = false;  // the next draw sets all of its own again
    bound_valid_key_ = false;  // The sky does not keep its key (CountChangePipeline)
    if (!sets_bound_) {
      dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 4,
                                   sets_.data(), 0, nullptr);
      sets_bound_ = true;
    }
    {
      dfn_.vkCmdPushConstants(cmd, layout_pipeline_,
                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(c.push), c.push);
    }
    dfn_.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 4, 1,
                                 &sets_ubo_[c.slot_ubo], 3, c.offsets_ubo.data());
    dfn_.vkCmdSetViewport(cmd, 0, 1, &c.viewport);
    dfn_.vkCmdSetScissor(cmd, 0, 1, &c.scissor);
    dfn_.vkCmdSetBlendConstants(cmd, c.blend);
    dfn_.vkCmdSetDepthBias(cmd, c.bias[0], 0.0f, c.bias[1]);
    if (c.with_stencil) {
      dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, c.stencil[0] & 0xFF);
      dfn_.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, c.stencil[1] & 0xFF);
      dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (c.stencil[0] >> 8) & 0xFF);
      dfn_.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, (c.stencil[1] >> 8) & 0xFF);
      dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, (c.stencil[0] >> 16) & 0xFF);
      dfn_.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, (c.stencil[1] >> 16) & 0xFF);
    }
    if (c.n_bindings) {
      std::array<VkBuffer, 16> buffers{};
      buffers.fill(c.buffer);
      dfn_.vkCmdBindVertexBuffers(cmd, 0, c.n_bindings, buffers.data(), c.offsets_vertices.data());
    }
    const uint32_t query = !diag_stats_draw_
                                  ? UINT32_MAX
                                  : context_->BeginStatsDraw(c.ps_more_one, c.category, c.vs_more_one);
    if (c.with_indices) {
      const VkIndexType type = c.indices_from_16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
      dfn_.vkCmdBindIndexBuffer(cmd, c.buffer, 0, type);
      dfn_.vkCmdDrawIndexed(cmd, c.indices, 1, c.first_index, -int32_t(c.vmin), 0);
    } else {
      dfn_.vkCmdDraw(cmd, c.count, 1, 0, 0);
    }
    if (query != UINT32_MAX) {
      context_->FinishStatsDraw(query);
    }
    ++draws_in_pass_;
    // Nothing tracked is valid any more: the dynamic state, the bindings and the index type are the sky's.
    // recorded_state_ = false makes the next draw also re-record stencil and indices.
    recorded_state_ = false;
    ubo_bound_ = false;
    recorded_bindings_ = 0;
    stencil_recorded_valid_ = false;
    indices_type_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  }

  // Adds the time since the mark to the stage and moves the mark to now.
  /*
   * Measurement only. What takes the time inside the ring's recording and pipeline stages (4.1 and 2.5 us per
   * recorded draw): each Vulkan command, PipelineFor and the shared constants block, on the same timed
   * draws (1 in 64). "C6 substages" line every 10 s: us per draw, us per call and calls per timed draw.
   */
  // Its lines, like those of the other ring reports, go to the report thread (MASSEFFECT_REPORT_RING).
  void ReportSubstages() {
    const auto now = std::chrono::steady_clock::now();
    if (now < sub_next_) {
      return;
    }
    const bool first = sub_next_ == std::chrono::steady_clock::time_point{};
    sub_next_ = now + std::chrono::seconds(10);
    if (first || !sub_samples_) {
      sub_ns_.fill(0);
      sub_n_.fill(0);
      sub_samples_ = 0;
      return;
    }
    static constexpr const char* kNames[19] = {
        "BindPipeline", "sets 0-3", "set 4 (UBO)", "Viewport", "Scissor", "BlendConstants", "DepthBias",
        "Stencil (6)", "BindVertexBuffers", "BindIndexBuffer", "Draw", "PipelineFor", "shared",
        "(unused)", "samplers (textures stage)",
        // The four segments of the "indices" stage (CutSubstage)
        "indices stage: targets and discards", "indices stage: depth bias",
        "indices stage: indices", "indices stage: vertices and diagnostics"};
    std::string line;
    double total = 0.0;
    for (size_t k = 0; k < 19; ++k) {
      if (!sub_n_[k]) {
        continue;
      }
      const double per_draw = double(sub_ns_[k]) / 1e3 / double(sub_samples_);
      total += per_draw;
      line += fmt::format(" | {} {:.2f} us/draw ({:.2f} us x {:.2f} per draw)", kNames[k], per_draw,
                           double(sub_ns_[k]) / 1e3 / double(sub_n_[k]),
                           double(sub_n_[k]) / double(sub_samples_));
    }
    MASSEFFECT_REPORT_RING("[native] C6 substages ({} timed draws; sum {:.2f} us per draw){}", sub_samples_,
                total, line);
    sub_ns_.fill(0);
    sub_n_.fill(0);
    sub_samples_ = 0;
  }

  /*
   * The base-zero path of a draw (masseffect_native_vertices_base_zero). Returns false if it has to bind as
   * usual. Self-checking guard: the first kBaseZeroToCheck draws, then 1 in 4,096, redo the computation
   * backwards in 64 bits (the first vertex the GPU reads falls on the first byte of the copy, with and
   * without indices), and check that vertexOffset fits in a signed 32-bit value and that the copy fits in
   * the buffer. On a single difference: that draw binds as usual, MISMATCH in the log and the path is off
   * for the session.
   */
  bool CheckBaseZero(VkDeviceSize offset, VkDeviceSize stride, uint64_t bytes, uint32_t vmin,
                         uint32_t& first_vertex) {
    const VkDeviceSize first = offset / stride;
    if (first > VkDeviceSize(INT32_MAX)) {
      return false;  // does not fit in firstVertex/vertexOffset (cannot happen with a 64 MB buffer)
    }
    const uint64_t n = ++base_checked_zero_;
    if (n <= kBaseZeroToCheck || (n & 4095) == 0) {
      const int64_t displacement = int64_t(first) - int64_t(vmin);  // the vertexOffset of indexed draws
      const bool ok = first * stride == offset &&
                        (displacement + int64_t(vmin)) * int64_t(stride) == int64_t(offset) &&
                        displacement >= int64_t(INT32_MIN) && displacement <= int64_t(INT32_MAX) &&
                        offset + bytes <= kUploadSize && offset + bytes <= upload_real_size_;
      if (!ok) {
        vertices_base_off_zero_ = true;
        vertices_base_zero_ = false;
        REXLOG_ERROR("[native] C6 zero-base vertices: MISMATCH (offset {} stride {} bytes {} vmin {} first {}): "
                     "this draw and the rest of the session bind as usual",
                     offset, stride, bytes, vmin, first);
        return false;
      }
      if (n == kBaseZeroToCheck) {
        MASSEFFECT_REPORT_RING("[native] C6 zero-base vertices: {} draws checked, 0 mismatches; still checking 1 in "
                    "every 4096", n);
      }
    }
    first_vertex = uint32_t(first);
    return true;
  }

  /*
   * Set 4 by differences, once per submission (UseSlot). Tells NVK whether it is wanted
   * (masseffect_native_set4_differences) and, if the NVK guard has seen a MISMATCH,
   * reports it in the log as an error once: NVK has already switched it off for the rest of the session.
   */
  void ControlSet4() {
    const bool request = REXCVAR_GET(masseffect_native_set4_differences);
    NvkSwitchSet4* const nvk = &nvk_switch_set4;
    const bool with_patch = nvk != nullptr && nvk->version == 1;
    if (request != set4_request_ || !set4_recorded_request_) {
      set4_request_ = request;
      set4_recorded_request_ = true;
      REXLOG_INFO("[native] C6 set 4 by differences: {} (frame {}){}",
                  request ? "requested from NVK" : "NOT requested: NVK binds the whole set", frame_,
                  with_patch ? "" : "; this NVK lacks patch_nvk_set4 (older Mesa or PC): binds as usual");
    }
    if (with_patch) {
      __atomic_store_n(&nvk->request, request ? 1 : 0, __ATOMIC_RELAXED);
      if (!set4_warned_difference_ && __atomic_load_n(&nvk->off, __ATOMIC_RELAXED) != 0) {
        set4_warned_difference_ = true;
        REXLOG_ERROR("[native] C6 set 4 by differences: MISMATCH seen by the NVK guard ({} cbufs wrongly "
                     "bound, each fixed in its own draw; details in rex_stderr.log). Switched off for the rest "
                     "of the session: NVK binds the whole set, as usual",
                     __atomic_load_n(&nvk->total.differences, __ATOMIC_RELAXED));
      }
    }
  }

  /*
   * The draw path in NVK, once per submission (like ControlSet4). Tells NVK what to measure and which
   * improvements are wanted (they apply from the next command buffer) and, if an improvement's guard has
   * seen a MISMATCH, reports it in the log as an error once: NVK has already switched it off for the
   * rest of the session.
   */
  void ControlDrawNvk() {
    NvkSwitchDraw* const nvk = &nvk_switch_draw;
    if (nvk == nullptr || nvk->version != 1) {
      nvk_preload_app_ = false;
      if (!nvk_recorded_draw_) {
        nvk_recorded_draw_ = true;
        REXLOG_INFO("[native] C6 NVK per draw: this NVK lacks the nvk_switch_draw contract (older "
                    "Mesa): no measurement and no improvements");
      }
      return;
    }
    int32_t measure = std::max<int32_t>(0, REXCVAR_GET(masseffect_native_nvk_measure));
    if (measure > 0) {
      measure = int32_t(std::bit_floor(uint32_t(measure)));  // NVK wants a power of 2
    }
    const int32_t requests[5] = {REXCVAR_GET(masseffect_native_nvk_emission) ? 1 : 0,
                                REXCVAR_GET(masseffect_native_nvk_cbufs) ? 1 : 0,
                                REXCVAR_GET(masseffect_native_nvk_dynamic) ? 1 : 0,
                                0,  // the fast set 4 path is not applied
                                REXCVAR_GET(masseffect_native_nvk_preload) ? 1 : 0};
    __atomic_store_n(&nvk->measure, measure, __ATOMIC_RELAXED);
    __atomic_store_n(&nvk->measure_failures, REXCVAR_GET(masseffect_native_nvk_measure_failures) ? 1 : 0, __ATOMIC_RELAXED);
    for (size_t m = 0; m < 5; ++m) {
      __atomic_store_n(&nvk->improvements[m].request, requests[m], __ATOMIC_RELAXED);
    }
    nvk_preload_app_ = requests[4] != 0;
    if (!nvk_recorded_draw_) {
      nvk_recorded_draw_ = true;
      REXLOG_INFO("[native] C6 NVK per draw: improvements requested (emission {}, cbufs {}, dynamic {}, prefetch {}); "
                  "measuring 1 in {} calls{}; environment NVK_SWITCH_DRAW {} (frame {})",
                  requests[0], requests[1], requests[2], requests[4], measure,
                  REXCVAR_GET(masseffect_native_nvk_measure_failures) ? " with cache misses separate" : "",
                  __atomic_load_n(&nvk->environment, __ATOMIC_RELAXED), frame_);
    }
    static constexpr const char* kNames[5] = {"emission", "cbufs", "dynamic", "fast set 4", "prefetch"};
    for (size_t m = 0; m < 5; ++m) {
      if (!nvk_off_warned_[m] && __atomic_load_n(&nvk->improvements[m].off, __ATOMIC_RELAXED) != 0) {
        nvk_off_warned_[m] = true;
        REXLOG_ERROR("[native] C6 NVK per draw: MISMATCH seen by the guard of '{}' ({} differing checks; "
                     "details in rex_stderr.log). Switched off for the rest of the session: NVK does it as "
                     "usual",
                     kNames[m], __atomic_load_n(&nvk->improvements[m].differences, __ATOMIC_RELAXED));
      }
    }
  }

  /*
   * Every 10 s, the NVK figures. Per part, us per measured call and us per measured draw; per measured
   * draw, how much work it carries; per pipeline bind, how many state copies changed nothing; and per
   * improvement, uses, checks and differences. Totals are from command buffers that have already finished.
   */
  void ReportDrawNvk() {
    const auto now = std::chrono::steady_clock::now();
    if (now - nvk_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = nvk_report_ == std::chrono::steady_clock::time_point{};
    nvk_report_ = now;
    NvkSwitchDraw* const nvk = &nvk_switch_draw;
    if (nvk == nullptr || nvk->version != 1) {
      return;
    }
    uint64_t times[16], ticks[16], counts[13], improvements[5][4];
    for (size_t i = 0; i < 16; ++i) {
      const uint64_t v = __atomic_load_n(&nvk->parts[i].times, __ATOMIC_RELAXED);
      const uint64_t t = __atomic_load_n(&nvk->parts[i].ticks, __ATOMIC_RELAXED);
      times[i] = v - nvk_previous_parts_[i][0];
      ticks[i] = t - nvk_previous_parts_[i][1];
      nvk_previous_parts_[i][0] = v;
      nvk_previous_parts_[i][1] = t;
    }
    for (size_t i = 0; i < 13; ++i) {
      const uint64_t c = __atomic_load_n(&nvk->counts[i], __ATOMIC_RELAXED);
      counts[i] = c - nvk_previous_counts_[i];
      nvk_previous_counts_[i] = c;
    }
    for (size_t m = 0; m < 5; ++m) {
      const uint64_t c[4] = {__atomic_load_n(&nvk->improvements[m].uses, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].validated, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].differences, __ATOMIC_RELAXED),
                             __atomic_load_n(&nvk->improvements[m].no_check, __ATOMIC_RELAXED)};
      for (size_t k = 0; k < 4; ++k) {
        improvements[m][k] = c[k] - nvk_previous_improvements_[m][k];
        nvk_previous_improvements_[m][k] = c[k];
      }
    }
    if (first) {
      return;
    }
    const uint64_t tps = __atomic_load_n(&nvk->ticks_per_second, __ATOMIC_RELAXED);
    const double us_tick = tps ? 1e6 / double(tps) : 0.0;
    const uint64_t draws = counts[0];  // NVK_SW_C_MEASURED_DRAWS
    // us per measured call and, in brackets, us per measured draw.
    const auto part = [&](size_t i) {
      return fmt::format("{:.2f} [{:.2f}]", times[i] ? double(ticks[i]) * us_tick / double(times[i]) : 0.0,
                         draws ? double(ticks[i]) * us_tick / double(draws) : 0.0);
    };
    const auto per_draw = [&](size_t i) { return draws ? double(counts[i]) / double(draws) : 0.0; };
    if (draws) {
      MASSEFFECT_REPORT_RING(
          "[native] C6 NVK parts (us per measured call [us per measured draw]; {} measured draws): "
          "whole draw {} | push desc {} | dynamic {} | touch shaders {} | shaders {} | cbufs {} | emit {} | "
          "BindPipeline: shaders {} touch {} state copy {} | sets {} (root {} dirty {}) | root table {} | "
          "new chunk {} | BindVertexBuffers {}",
          draws, part(0), part(1), part(2), part(3), part(4), part(5), part(6), part(7), part(8),
          part(9), part(10), part(11), part(12), part(13), part(14), part(15));
      MASSEFFECT_REPORT_RING(
          "[native] C6 NVK per draw, measured: {:.1f} dwords; {:.2f} with dirty dynamic state ({:.1f} bits); {:.2f} with "
          "dirty shaders; {:.2f} cbufs rebound | pipelines: {} binds measured, {:.1f} % copies that changed "
          "nothing, {:.2f} new bits per bind | {} root-table uploads ({:.1f} words each), {} new chunks",
          per_draw(1), per_draw(2), counts[2] ? double(counts[3]) / double(counts[2]) : 0.0, per_draw(4),
          per_draw(5), counts[6], counts[6] ? 100.0 * double(counts[7]) / double(counts[6]) : 0.0,
          counts[6] ? double(counts[8]) / double(counts[6]) : 0.0, counts[10],
          counts[10] ? double(counts[11]) / double(counts[10]) : 0.0, counts[12]);
    }
    static constexpr size_t kShown[4] = {0, 1, 2, 4};  // without the fast set 4 path, which is not applied
    static constexpr const char* kNames[5] = {"emission", "cbufs", "dynamic", "fast set 4", "prefetch"};
    std::string text;
    for (const size_t m : kShown) {
      const char* state = __atomic_load_n(&nvk->improvements[m].off, __ATOMIC_RELAXED) != 0 ? "SWITCHED OFF by the guard"
                           : __atomic_load_n(&nvk->improvements[m].request, __ATOMIC_RELAXED) == 0 ? "not requested"
                                                                                              : "on";
      text += fmt::format(" | {} ({}): {} uses, {} checked equal, {} differing, {} unchecked", kNames[m],
                           state, improvements[m][0], improvements[m][1], improvements[m][2], improvements[m][3]);
    }
    MASSEFFECT_REPORT_RING("[native] C6 NVK improvements (last 10 s){}", text);
  }

  /*
   * Every 10 s, set 4. What changes at each bind (measured here, with or without the NVK patch) and, if
   * NVK has it, what is saved: root table writes per bind (4 without it), cbufs not rebound and the state
   * of its guard.
   */
  void ReportSet4() {
    const auto now = std::chrono::steady_clock::now();
    if (now - set4_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = set4_report_ == std::chrono::steady_clock::time_point{};
    set4_report_ = now;
    uint64_t bindings = set4_first_;
    for (const uint64_t n : set4_changes_) {
      bindings += n;
    }
    if (!first && set4_draws_ && bindings) {
      const auto pct = [bindings](uint64_t n) { return 100.0 * double(n) / double(bindings); };
      MASSEFFECT_REPORT_RING(
          "[native] C6 set 4: {:.2f} binds per draw ({} in {} draws); changes only VS {:.1f} %, only PS "
          "{:.1f} %, only shared {:.1f} %, VS+PS {:.1f} %, VS+shared {:.1f} %, PS+shared {:.1f} %, "
          "all three {:.1f} %, after buffer, slot or sky {:.1f} % | constants uploaded again: VS {} by "
          "generation, {} by epoch and {} because the shader reads more; PS {}, {} and {}",
          double(bindings) / double(set4_draws_), bindings, set4_draws_, pct(set4_changes_[1]),
          pct(set4_changes_[2]), pct(set4_changes_[4]), pct(set4_changes_[3]), pct(set4_changes_[5]),
          pct(set4_changes_[6]), pct(set4_changes_[7]), pct(set4_first_), reuploaded_vs_[0], reuploaded_vs_[1],
          reuploaded_vs_[2], reuploaded_ps_[0], reuploaded_ps_[1], reuploaded_ps_[2]);
      if (NvkSwitchSet4* const nvk = &nvk_switch_set4; nvk != nullptr && nvk->version == 1) {
        const uint64_t counts[9] = {__atomic_load_n(&nvk->total.bindings_difference, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.complete_bindings, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.writes_root, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.dwords_root, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.cbufs_dirty, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.cbufs_saved, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.draws, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.checks, __ATOMIC_RELAXED),
                                     __atomic_load_n(&nvk->total.differences, __ATOMIC_RELAXED)};
        uint64_t d[9];
        for (size_t i = 0; i < 9; ++i) {
          d[i] = counts[i] - set4_nvk_previous_[i];
          set4_nvk_previous_[i] = counts[i];
        }
        const char* const state = __atomic_load_n(&nvk->off, __ATOMIC_RELAXED) != 0 ? "SWITCHED OFF by the guard"
                                   : __atomic_load_n(&nvk->environment, __ATOMIC_RELAXED) == 0
                                       ? "switched off by NVK_SWITCH_DYN_UBO_DELTA"
                                   : !set4_request_ ? "switched off by the cvar"
                                                   : "on";
        MASSEFFECT_REPORT_RING(
            "[native] C6 set 4 by differences (NVK, {}): {} binds by differences and {} full; {:.2f} writes "
            "to the root table per bind (4 before) with {:.2f} words; cbufs dirtied {} and not dirtied {} ({:.1f} % "
            "fewer); guard: {} cbufs checked ({} draws through the new path), {} mismatches",
            state, d[0], d[1], d[0] ? double(d[2]) / double(d[0]) : 0.0, d[0] ? double(d[3]) / double(d[0]) : 0.0,
            d[4], d[5], d[4] + d[5] ? 100.0 * double(d[5]) / double(d[4] + d[5]) : 0.0, d[7], d[6], d[8]);
      }
    }
    set4_changes_.fill(0);
    set4_first_ = 0;
    set4_draws_ = 0;
    reuploaded_vs_.fill(0);
    reuploaded_ps_.fill(0);
  }

  // Every 10 s, how many draws go without binding their own offset (base zero).
  void ReportBaseZero() {
    const auto now = std::chrono::steady_clock::now();
    if (now - base_zero_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = base_zero_report_ == std::chrono::steady_clock::time_point{};
    base_zero_report_ = now;
    if (!first && base_zero_total_) {
      MASSEFFECT_REPORT_RING("[native] C6 zero-base vertices ({}): {} of {} draws with vertices not bound at their own "
                  "offset ({:.1f} %); {} with several binds and {} with the dedupe copy outside a "
                  "multiple of the stride; {:.2f} vkCmdBindVertexBuffers per draw",
                  vertices_base_off_zero_ ? "SWITCHED OFF by the guard"
                  : vertices_base_zero_       ? "on"
                                              : "off",
                  base_zero_draws_, base_zero_total_, 100.0 * double(base_zero_draws_) / double(base_zero_total_),
                  base_zero_several_bindings_, base_misaligned_zero_,
                  double(base_zero_recorded_bindings_) / double(base_zero_total_));
    }
    base_zero_draws_ = 0;
    base_zero_total_ = 0;
    base_zero_several_bindings_ = 0;
    base_misaligned_zero_ = 0;
    base_zero_recorded_bindings_ = 0;
  }

  /*
   * The framing of a draw, moved out of Draw unchanged (masseffect_native_framing_cache). Returns 0 if it
   * has to draw, 1 if the viewport is empty and 2 if the scissor is empty (in both cases Draw returns
   * without drawing, as before; the scissor is left at zero). Same operations in the same order. The only
   * multiplications GCC could fuse with an add or subtract (width * 0.5 and height * 0.5) are exact, so
   * fused or not they give the same bits.
   */
  uint32_t ComputeFraming(const uint32_t* r, uint32_t vte, bool no_clip, uint32_t sc_mode, VkViewport& viewport,
                            float ndc[4], VkRect2D& scissor, me::native::Msaa2PhaseProbe phase_probe) {
    float scale_x = (vte & 0x1) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XSCALE]) : 1.0f;
    float center_x = (vte & 0x2) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_XOFFSET]) : 0.0f;
    float scale_y = (vte & 0x4) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YSCALE]) : 1.0f;
    float center_y = (vte & 0x8) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_YOFFSET]) : 0.0f;
    const float scale_z = (vte & 0x10) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZSCALE]) : 1.0f;
    const float center_z = (vte & 0x20) ? Float(r[gr::XE_GPU_REG_PA_CL_VPORT_ZOFFSET]) : 0.0f;
    const uint32_t window = r[gr::XE_GPU_REG_PA_SC_WINDOW_OFFSET];
    const int32_t window_x = int32_t((window & 0x7FFF) << 17) >> 17;
    const int32_t window_y = int32_t(((window >> 16) & 0x7FFF) << 17) >> 17;
    if ((sc_mode >> 16) & 0x1) {
      center_x += float(window_x);
      center_y += float(window_y);
    }
    if (phase_probe.active) {
      center_x += phase_probe.geometry_x;
      center_y += phase_probe.geometry_y;
    }
    // With the shadow map drawn smaller (masseffect_native_shadows_scale), the guest still speaks in
    // 1600-pixel units. The viewport and the scissor are multiplied by the pass scale, and with that the
    // geometry lands where it should; the rest (ndc, half pixel) already comes from the pass size.
    if (pass_scale_ != 1.0f) {
      scale_x *= pass_scale_;
      center_x *= pass_scale_;
      scale_y *= pass_scale_;
      center_y *= pass_scale_;
    }
    scale_x *= pass_raster_scale_x_;
    center_x *= pass_raster_scale_x_;
    viewport = VkViewport{};
    viewport.x = center_x - std::abs(scale_x);
    viewport.width = 2.0f * std::abs(scale_x);
    // A negative YSCALE (the D3D norm) gives a positive height; a positive one, an inverted height.
    viewport.y = center_y + scale_y;
    viewport.height = -2.0f * scale_y;
    if (!((r[gr::XE_GPU_REG_PA_CL_CLIP_CNTL] >> 19) & 0x1)) {
      Warn(14, "OpenGL clip space (dx_clip_space_def = 0): Z not adjusted");
    }
    // Scale BEFORE clamping: guest 1.5 must become host .75, not .5.
    viewport.minDepth = std::clamp(me::native::GuestDepthToNative(center_z, pass_depth_float24_half_), 0.0f, 1.0f);
    viewport.maxDepth = std::clamp(me::native::GuestDepthToNative(center_z + scale_z, pass_depth_float24_half_), 0.0f, 1.0f);
    // g_NdcScale (x, y) and g_NdcOffset (x, y): identity when clipping is on.
    ndc[0] = 1.0f;
    ndc[1] = 1.0f;
    ndc[2] = 0.0f;
    ndc[3] = 0.0f;
    if (no_clip) {
      const float width = float(pass_width_);
      const float height = float(pass_height_);
      ndc[0] = scale_x * 2.0f / width;
      ndc[2] = (center_x - width * 0.5f) * 2.0f / width;
      // DXC flips Y at the end of the VS (-fvk-invert-y): the D3D Y goes here.
      ndc[1] = -scale_y * 2.0f / height;
      ndc[3] = -(center_y - height * 0.5f) * 2.0f / height;
      viewport.x = 0.0f;
      viewport.y = 0.0f;
      viewport.width = width;
      viewport.height = height;
    }
    scissor = VkRect2D{};
    if (viewport.width < 1.0f || std::abs(viewport.height) < 1.0f) {
      return 1;
    }
    const uint32_t tl = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL];
    const uint32_t br = r[gr::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR];
    int32_t x0 = int32_t(tl & 0x3FFF), y0 = int32_t((tl >> 16) & 0x3FFF);
    int32_t x1 = int32_t(br & 0x3FFF), y1 = int32_t((br >> 16) & 0x3FFF);
    if (!((tl >> 31) & 0x1)) {
      x0 += window_x;
      y0 += window_y;
      x1 += window_x;
      y1 += window_y;
    }
    const float scale_scissor_x = pass_scale_ * pass_raster_scale_x_;
    if (scale_scissor_x != 1.0f) {
      x0 = int32_t(std::floor(float(x0) * scale_scissor_x));
      x1 = int32_t(std::ceil(float(x1) * scale_scissor_x));
    }
    if (pass_scale_ != 1.0f) {  // Y is shadow resolution only, never grid expansion.
      y0 = int32_t(std::floor(float(y0) * pass_scale_));
      y1 = int32_t(std::ceil(float(y1) * pass_scale_));
    }
    x0 = std::clamp(x0, 0, int32_t(pass_width_));
    x1 = std::clamp(x1, 0, int32_t(pass_width_));
    y0 = std::clamp(y0, 0, int32_t(pass_height_));
    y1 = std::clamp(y1, 0, int32_t(pass_height_));
    if (x1 <= x0 || y1 <= y0) {
      return 2;
    }
    scissor = VkRect2D{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    return 0;
  }

  // Guard of masseffect_native_framing_cache. The current computation against the saved one, byte by byte.
  void CompareFraming(const VkViewport& viewport, const float ndc[4], const VkRect2D& scissor, uint32_t empty) {
    ++checked_framing_;
    const bool equal = empty == empty_framing_ &&
                       Equal(&viewport, &framing_viewport_, sizeof(viewport)) &&
                       Equal(ndc, framing_ndc_, sizeof(framing_ndc_)) &&
                       Equal(&scissor, &framing_scissor_, sizeof(scissor));
    if (!equal) {
      framing_cache_off_ = true;
      framing_cache_ = false;
      REXLOG_ERROR("[native] C6 cached framing: MISMATCH at check {} (empty {} / {}; viewport {},{} "
                   "{}x{} / {},{} {}x{}; scissor {},{} {}x{} / {},{} {}x{}). Switched off for the rest of the session: this "
                   "draw uses the recomputed one",
                   checked_framing_, empty, empty_framing_, viewport.x, viewport.y, viewport.width,
                   viewport.height, framing_viewport_.x, framing_viewport_.y, framing_viewport_.width,
                   framing_viewport_.height, scissor.offset.x, scissor.offset.y, scissor.extent.width,
                   scissor.extent.height, framing_scissor_.offset.x, framing_scissor_.offset.y,
                   framing_scissor_.extent.width, framing_scissor_.extent.height);
    } else if (checked_framing_ == kFramingsToCheck) {
      MASSEFFECT_REPORT_RING("[native] C6 cached framing: {} hits checked against the computation, 0 mismatches; still "
                  "checking 1 in 4096", checked_framing_);
    }
  }

  /*
   * The util_height_ element for that pitch (masseffect_native_util_height_memo). The pitch is the pass's and only
   * changes with the pass, so the last one is remembered. References to unordered_map elements are not
   * invalidated by insertion (only by erasure, and util_height_ never erases). Guard: the first
   * kUtilHeightToCheck reuses, then 1 in 4,096, also look it up in the map and must get the same element.
   */
  uint32_t& UtilHeightFor(uint32_t pitch) {
    if (!util_height_memo_active_) {
      return util_height_[pitch];
    }
    if (util_height_memo_ == nullptr || pitch != util_height_memo_pitch_) {
      util_height_memo_ = &util_height_[pitch];
      util_height_memo_pitch_ = pitch;
      return *util_height_memo_;
    }
    const uint64_t n = ++util_height_memo_hits_;
    if (n <= kUtilHeightToCheck || (n & 4095) == 0) {
      uint32_t* const of_the_map = &util_height_[pitch];
      if (of_the_map != util_height_memo_) {
        util_height_memo_off_ = true;
        util_height_memo_active_ = false;
        util_height_memo_ = nullptr;
        REXLOG_ERROR("[native] C6 remembered usable height: MISMATCH (pitch {}, check {}): not the map's "
                     "element. Switched off for the rest of the session", pitch, n);
        return *of_the_map;
      }
      if (n == kUtilHeightToCheck) {
        MASSEFFECT_REPORT_RING("[native] C6 remembered usable height: {} checks against the map, 0 mismatches; still "
                    "checking 1 in 4096", n);
      }
    }
    return *util_height_memo_;
  }

  /*
   * Guard of masseffect_native_key_fast_pass. With the same render target bytes, the XXH3 must be pass_key_. Returns the computed
   * one, which is the one that decides for this draw.
   */
  uint64_t CheckPassKey(const uint64_t keys[5], uint64_t n) {
    const uint64_t computed = XXH3_64bits(keys, sizeof(uint64_t) * 5);
    if (computed != pass_key_) {
      key_fast_off_pass_ = true;
      key_fast_pass_ = false;
      REXLOG_ERROR("[native] C6 pass key: MISMATCH (check {}: {:016X} computed, {:016X} stored). "
                   "Switched off for the rest of the session", n, computed, pass_key_);
    } else if (n == kKeysPassToCheck) {
      MASSEFFECT_REPORT_RING("[native] C6 pass key: {} reused keys checked with XXH3, 0 mismatches; still "
                  "checking 1 in 4096", n);
    }
    return computed;
  }

  // Every 10 s, how much the small per-draw savings save.
  void ReportMinutiae() {
    const auto now = std::chrono::steady_clock::now();
    if (now - minutiae_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = minutiae_report_ == std::chrono::steady_clock::time_point{};
    minutiae_report_ = now;
    const uint64_t framings = framing_hits_report_ + framing_computations_report_;
    if (!first && framings) {
      MASSEFFECT_REPORT_RING("[native] C6 small savings: cached framing in {} of {} draws ({:.1f} %; {}{}); "
                  "pass key without XXH3 {} times ({}); remembered usable height {} times ({}); settings of "
                  "color-less draws {}",
                  framing_hits_report_, framings,
                  100.0 * double(framing_hits_report_) / double(framings),
                  framing_cache_off_ ? "SWITCHED OFF by the guard" : framing_cache_ ? "on" : "off",
                  checked_framing_ >= kFramingsToCheck ? ", guard passed" : ", checking",
                  keys_fast_pass_ - keys_fast_previous_pass_,
                  key_fast_off_pass_ ? "SWITCHED OFF by the guard" : key_fast_pass_ ? "on" : "off",
                  util_height_memo_hits_ - util_height_memo_previous_hits_,
                  util_height_memo_off_ ? "SWITCHED OFF by the guard" : util_height_memo_active_ ? "on" : "off",
                  cvars_per_frame_ ? "once per frame" : "on every draw");
    }
    framing_hits_report_ = 0;
    framing_computations_report_ = 0;
    keys_fast_previous_pass_ = keys_fast_pass_;
    util_height_memo_previous_hits_ = util_height_memo_hits_;
  }

  // The guard of the direct-mapped pipeline cache. The same key in the map (the usual path) must give the
  // same VkPipeline as the slot. Otherwise it switches off and returns false.
  bool CheckCellPipeline(uint64_t fingerprint, const PipelineKey& key, VkPipeline in_cell, uint64_t n) {
    ++pipelines_direct_checked_;
    VkPipeline of_the_map = VK_NULL_HANDLE;
    if (const auto it = pipelines_.find(fingerprint);
        it != pipelines_.end() && Equal(&it->second.first, &key, sizeof(key))) {
      of_the_map = it->second.second;
    }
    if (of_the_map != in_cell) {
      pipelines_direct_off_ = true;
      pipelines_direct_ = false;
      REXLOG_ERROR("[native] C6 pipelines: MISMATCH between the direct cache and the map (check {}, VS {} PS {} "
                   "specialization {:08X}). Switched off for the rest of the session: the map rules",
                   n, key.vs, key.ps, key.specialization);
      return false;
    }
    if (n == kPipelinesToCheck) {
      MASSEFFECT_REPORT_RING("[native] C6 pipelines: {} direct-cache hits checked against the map, 0 "
                  "mismatches; still checking 1 in 4096", n);
    }
    return true;
  }

  /*
   * Measurement only (masseffect_native_count_changes_pipeline). What changes at each vkCmdBindPipeline.
   *
   * BindPipeline costs 3.3-4.5 us per call in play (0.3-0.4 per draw), and on each one NVK copies all
   * the pipeline's fixed state (vk_dynamic_graphics_state_copy, ~80 groups). If the change is state only,
   * with dynamic state (vkCmdSet*) no new pipeline would be needed. Before touching anything, the counts
   * and kinds have to be known:
   *  - cull mode, topology within the same class, Z test/write/function, stencil, bias and restart are
   *    EDS1/EDS2, core in Vulkan 1.3: the console (API 1.3.354) already provides them without touching
   *    the SDK;
   *  - blending, color masks and topology of another class need VK_EXT_extended_dynamic_state3, which NVK
   *    exposes on Maxwell and the SDK enables.
   * The new key is compared with the last one bound in the same command buffer, field by field and
   * without XXH3. Blending, masks and depth are compared in canonical form (CanonicalState: only what
   * PipelineFor really reads). If only bits PipelineFor does not use differ, the pipeline is effectively the
   * same: no effect. Cost: an 80-byte memcmp and about 60 operations per bind (0.3-0.4 binds per draw). It
   * changes nothing.
   */
  static uint32_t ClassTopology(uint32_t topology) {
    switch (topology) {
      case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
        return 0;
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
      case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
        return 1;
      case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return 3;
      default:
        return 2;  // triangles: list, strip and fan
    }
  }

  // What PipelineFor really reads from a key's blending, masks and depth, with its same criterion
  // (blendEnable per target; depthWriteEnable = test and write; the back face copies the front).
  static void CanonicalState(const PipelineKey& c, uint32_t blend[4], uint32_t& masks, uint32_t& depth) {
    masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      blend[i] = 0;
      if (!c.formats[i]) {
        continue;  // PipelineFor skips color targets not in the pass
      }
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      masks |= mask << (i * 4);
      const uint32_t m = c.blend[i];
      const bool color_direct = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool direct_alpha = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      if (((mask & 0x7) && !color_direct) || ((mask & 0x8) && !direct_alpha)) {
        blend[i] = m;  // blendEnable: factors count (without blending Vulkan ignores them)
      }
    }
    uint32_t d = c.formats[4] ? c.depth : 0;  // without a depth target PipelineFor ignores it
    d &= ~0x8u;                                      // PipelineFor does not read bit 3
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;  // without a Z test, write and function do not count
    }
    if (!(d & 0x1)) {
      d &= 0x77u;  // without stencil, its functions and operations do not count
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
    }
    depth = d;
  }

  // Classifies a vkCmdBindPipeline from Draw against the last key bound in this buffer.
  // after_begin_pass: BeginPass had forgotten the bound pipeline (pipeline_bound_ set to
  // VK_NULL_HANDLE).
  void CountChangePipeline(const PipelineKey& new_entry, bool after_begin_pass) {
    uint64_t* const n = changes_pipeline_.data();
    ++n[kChangeBindings];
    n[kChangeAfterPass] += after_begin_pass ? 1 : 0;
    const PipelineKey& a = bound_key_;
    constexpr uint32_t kBitsTestAlpha = 0x2u | (0x7u << kSpecFunctionAlphaDisplacement);
    if (!bound_valid_key_) {
      ++n[kFirstChange];
    } else if (Equal(&a, &new_entry, sizeof(new_entry))) {
      ++n[kIdenticalChange];  // only after BeginPass: Vulkan kept the bound pipeline across passes
    } else if (a.vs != new_entry.vs || a.ps != new_entry.ps) {
      ++n[kChangeShaders];
    } else if (a.entry != new_entry.entry) {
      ++n[kChangeEntry];
    } else if (!Equal(a.formats, new_entry.formats, sizeof(a.formats))) {
      ++n[kChangeFormats];
    } else if (const uint32_t spec = a.specialization ^ new_entry.specialization; spec != 0) {
      ++n[(spec & ~kBitsTestAlpha) == 0                      ? kChangeSpecAlpha
          : (spec & ~(kBitsTestAlpha | kSpecZEarly)) == 0 ? kChangeSpecZEarly
                                                              : kChangeSpecOther];
    } else {
      // Same shaders, input, formats and specialization: only fixed pipeline state changes.
      uint32_t blend_a[4], blend_n[4], masks_a, masks_n, prof_a, prof_n;
      CanonicalState(a, blend_a, masks_a, prof_a);
      CanonicalState(new_entry, blend_n, masks_n, prof_n);
      const bool topology = a.topology != new_entry.topology;
      const bool class_value = ClassTopology(a.topology) != ClassTopology(new_entry.topology);
      const bool blend = !Equal(blend_a, blend_n, sizeof(blend_a));
      const bool masks = masks_a != masks_n;
      const uint32_t prof = prof_a ^ prof_n;
      const uint32_t rast = a.rasterization ^ new_entry.rasterization;
      if (!topology && !blend && !masks && !prof && !rast) {
        ++n[kChangeNoEffect];  // only bits PipelineFor does not read: effectively the same pipeline
        n[kChangeNoEffectAfterPass] += after_begin_pass ? 1 : 0;
      } else {
        const bool eds12 = !blend && !masks && !class_value;
        ++n[kChangeOnlyState];
        n[kChangeAfterStatePass] += after_begin_pass ? 1 : 0;
        n[kChangeEds12] += eds12 ? 1 : 0;
        n[kChangeEds12AfterPass] += (eds12 && after_begin_pass) ? 1 : 0;
        n[kFieldTopology] += topology ? 1 : 0;
        n[kFieldClassTopology] += class_value ? 1 : 0;
        n[kFieldBlend] += blend ? 1 : 0;
        n[kFieldMasks] += masks ? 1 : 0;
        n[kFieldZ] += (prof & 0x76u) ? 1 : 0;
        n[kFieldStencil] += (prof & ~0x76u) ? 1 : 0;
        n[kFieldFace] += (rast & 0x7u) ? 1 : 0;
        n[kFieldReset] += (rast & 0x8u) ? 1 : 0;
        n[kFieldBias] += (rast & 0x10u) ? 1 : 0;
      }
    }
    bound_key_ = new_entry;
    bound_valid_key_ = true;
  }

  // Every 20 s, what changes at each vkCmdBindPipeline of the ring (measurement only). Reads its cvar on
  // every call: ReportPipelinesDirect calls it, once per frame.
  void ReportChangesPipeline(std::chrono::steady_clock::time_point now) {
    const bool count = REXCVAR_GET(masseffect_native_count_changes_pipeline);
    if (count != count_changes_pipeline_) {
      count_changes_pipeline_ = count;
      bound_valid_key_ = false;  // what was tracked while not counting cannot be compared
    }
    if (now - changes_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = changes_report_ == std::chrono::steady_clock::time_point{};
    const double seconds = std::chrono::duration<double>(now - changes_report_).count();
    changes_report_ = now;
    const uint64_t draws = drawn_ - changes_previous_draws_;
    const uint64_t frames = frame_ - changes_previous_frames_;
    changes_previous_draws_ = drawn_;
    changes_previous_frames_ = frame_;
    const std::array<uint64_t, kChangesN> n = changes_pipeline_;
    changes_pipeline_.fill(0);
    if (first || !count_changes_pipeline_ || !n[kChangeBindings] || !draws || !frames) {
      return;
    }
    const double bindings = double(n[kChangeBindings]);
    const double f = double(frames);
    const auto per_draw = [&](uint64_t x) { return double(x) / double(draws); };
    const auto pct = [&](uint64_t x) { return 100.0 * double(x) / bindings; };
    const uint64_t spec = n[kChangeSpecAlpha] + n[kChangeSpecZEarly] + n[kChangeSpecOther];
    MASSEFFECT_REPORT_RING(
        "[native] C6 pipeline changes (measurement only): {} vkCmdBindPipeline in {:.0f} s ({:.3f} per "
        "draw, {:.1f} per frame; {} are the first of a pass) | no previous key {} | the SAME key after "
        "starting a pass {} ({:.1f} %) | shaders change {} ({:.1f} %) | other entry {} | other formats {} | "
        "specialization {} ({:.1f} %: alpha test {}, with early Z {}, other {}) | no effect (bits that "
        "PipelineFor does not read) {} ({:.1f} %) | STATE ONLY {} ({:.1f} %, {:.3f} per draw): EDS1/EDS2 without touching the SDK "
        "{}, the rest needs EDS3",
        n[kChangeBindings], seconds, per_draw(n[kChangeBindings]), bindings / f, n[kChangeAfterPass],
        n[kFirstChange], n[kIdenticalChange], pct(n[kIdenticalChange]), n[kChangeShaders], pct(n[kChangeShaders]),
        n[kChangeEntry], n[kChangeFormats], spec, pct(spec), n[kChangeSpecAlpha], n[kChangeSpecZEarly],
        n[kChangeSpecOther], n[kChangeNoEffect], pct(n[kChangeNoEffect]), n[kChangeOnlyState],
        pct(n[kChangeOnlyState]), per_draw(n[kChangeOnlyState]), n[kChangeEds12]);
    // Avoidable binds per frame with each fix, each one separately (those of the first draw of a pass are
    // not avoided by dynamic state or by the canonical key while BeginPass forgets the bound pipeline).
    constexpr double kUsPerBinding = 3.0;  // BindPipeline: 3.3-4.5 us per call in play
    const uint64_t canonical = n[kChangeNoEffect] - n[kChangeNoEffectAfterPass];
    const uint64_t eds12 = n[kChangeEds12] - n[kChangeEds12AfterPass];
    const uint64_t eds123 = n[kChangeOnlyState] - n[kChangeAfterStatePass];
    MASSEFFECT_REPORT_RING(
        "[native] C6 pipeline changes, state-only fields: topology {} (of class {}), blend {}, masks {}, "
        "Z {}, stencil {}, face {}, restart {}, bias {}; {} of them the first of a pass | avoidable binds per "
        "frame (at ~3 us each): {:.1f} with the canonical key ({:.2f} ms), {:.1f} without forgetting the pipeline at "
        "pass start ({:.2f} ms), {:.1f} with EDS1/EDS2 ({:.2f} ms), {:.1f} with EDS1/EDS2/EDS3 ({:.2f} ms)",
        n[kFieldTopology], n[kFieldClassTopology], n[kFieldBlend], n[kFieldMasks], n[kFieldZ],
        n[kFieldStencil], n[kFieldFace], n[kFieldReset], n[kFieldBias], n[kChangeAfterStatePass],
        double(canonical) / f, double(canonical) / f * kUsPerBinding / 1000.0, double(n[kIdenticalChange]) / f,
        double(n[kIdenticalChange]) / f * kUsPerBinding / 1000.0, double(eds12) / f,
        double(eds12) / f * kUsPerBinding / 1000.0, double(eds123) / f, double(eds123) / f * kUsPerBinding / 1000.0);
  }

  // Every 10 s, the hit rate of the direct-mapped pipeline cache.
  void ReportPipelinesDirect() {
    const auto now = std::chrono::steady_clock::now();
    ReportChangesPipeline(now);  // reads its cvar every frame and writes every 20 s
    TryPrewarm();       // starts the thread as soon as the library is loaded
    PrewarmedReport(now);  // every 10 s, if there is anything new, and its guard
    if (now - pipelines_direct_report_ < std::chrono::seconds(10)) {
      return;
    }
    const bool first = pipelines_direct_report_ == std::chrono::steady_clock::time_point{};
    pipelines_direct_report_ = now;
    const uint64_t hits = pipelines_direct_hits_ - pipelines_direct_previous_hits_;
    const uint64_t failures = pipelines_direct_failures_ - pipelines_direct_previous_failures_;
    pipelines_direct_previous_hits_ = pipelines_direct_hits_;
    pipelines_direct_previous_failures_ = pipelines_direct_failures_;
    if (!first && (hits || failures)) {
      MASSEFFECT_REPORT_RING("[native] C6 pipelines: direct cache {} hits and {} misses ({:.1f} % of the "
                  "lookups the single-entry shortcut does not resolve); {} checked against the map ({})",
                  hits, failures, 100.0 * double(hits) / double(hits + failures),
                  pipelines_direct_checked_,
                  pipelines_direct_off_ ? "SWITCHED OFF by the guard"
                  : pipelines_direct_checked_ >= kPipelinesToCheck ? "guard passed"
                                                                           : "checking");
    }
  }

  /*
   * The "indices" stage once rose from ~1.4 to ~2.6 us per draw without any change to its code. Four
   * cuts inside it (C6 substages 15-18), only on the timed draws (1 in 64): they cost four clock reads on
   * those draws, so the "indices" stage reads ~0.2-0.3 us higher than without them.
   */
  void CutSubstage(size_t k, std::chrono::steady_clock::time_point& t) {
    if (!time_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    sub_ns_[k] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count());
    ++sub_n_[k];
    t = now;
  }

  void Stage(size_t stage, std::chrono::steady_clock::time_point& mark) {
    if (!time_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    stages_ns_[stage] += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count());
    mark = now;
  }

  // Diagnostic masseffect_native_diag_vertices_repeated: vertex bytes that repeat address, size, byte order
  // and content within the same frame or relative to an earlier frame.
  void NoteVerticesRepeated(uint64_t address, uint32_t bytes, uint32_t order,
                               const uint8_t* data) {
    const auto before = std::chrono::steady_clock::now();
    const uint64_t fingerprint = XXH3_64bits(data, bytes);
    const uint64_t key =
        XXH3_64bits_withSeed(&address, sizeof(address), (uint64_t(bytes) << 2) | order);
    if (vertices_seen_.size() > 200000) {
      vertices_seen_.clear();  // memory cap of the diagnostic
    }
    VerticesSeen& seen = vertices_seen_[key];
    if (seen.fingerprint == fingerprint && seen.frame == frame_) {
      bytes_repeated_frame_ += bytes;
    } else if (seen.fingerprint == fingerprint && seen.frame != UINT64_MAX &&
               seen.frame < frame_) {
      bytes_equal_previous_ += bytes;
    }
    seen.frame = frame_;
    seen.fingerprint = fingerprint;
    bytes_recorded_ += bytes;
    if (before - repeated_report_ >= std::chrono::seconds(10)) {
      repeated_report_ = before;
      REXLOG_INFO("[native] ME repeated vertices (10 s): {:.1f} MB seen; same range and contents within the "
                  "frame {:.1f} MB ({:.0f} %), same as in an earlier frame {:.1f} MB ({:.0f} %), "
                  "{} ranges remembered",
                  double(bytes_recorded_) / 1048576.0, double(bytes_repeated_frame_) / 1048576.0,
                  100.0 * double(bytes_repeated_frame_) / double(std::max<uint64_t>(1, bytes_recorded_)),
                  double(bytes_equal_previous_) / 1048576.0,
                  100.0 * double(bytes_equal_previous_) / double(std::max<uint64_t>(1, bytes_recorded_)),
                  vertices_seen_.size());
      bytes_recorded_ = bytes_repeated_frame_ = bytes_equal_previous_ = 0;
    }
    ns_hash_vertices_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - before)
                                      .count());
  }

  void NotifyGraphicsExternalState() override {
    pipeline_bound_ = VK_NULL_HANDLE;
    eds_valid_ = false;
    bound_valid_key_ = false;
    sets_bound_ = false;
    ubo_bound_ = false;
    recorded_state_ = false;
    recorded_bindings_ = 0;
    stencil_recorded_valid_ = false;
    indices_type_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  }

  void FinishPass() override {
    if (active_pass_) {
      /*
       * Fallback path of the deferred sky. If nothing has emitted it yet (no transparent draw arrived, or the
       * pass closes because of a copy, a resolve or a submission), it is emitted here, inside the pass and
       * before vkCmdEndRenderPass. Losing the sky would be a visible and serious bug, so this is the last
       * place it can be, and it is always reached: the command buffer is only closed through SendWork
       * -> BeforeSend -> FinishPass.
       */
      if (pending_sky_) {
        EmitPostponedSky(pass_commands_, kSkyPerPassEnd);
      }
      const auto before_end = std::chrono::steady_clock::now();
      dfn_.vkCmdEndRenderPass(pass_commands_);
      if (stats_pass_ != UINT32_MAX) {
        context_->FinishStats(stats_pass_);
        stats_pass_ = UINT32_MAX;
      }
      ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - before_end).count());
      const uint32_t closed_category = category_pass_;
      active_pass_ = false;
      // Just in case. A pending sky with the pass already closed cannot be recorded; it is counted so it
      // shows up in the report instead of disappearing silently.
      if (pending_sky_) {
        pending_sky_ = false;
        ++lost_sky_;
      }
      // As soon as the shadow pass closes, submit to the GPU. Once per frame: the flag keeps reopened passes
      // from submitting again. SendAndWait submits and keeps recording in the next slot; it waits for
      // nothing (the name is misleading).
      if (closed_category == kGpuShadows && !sent_after_shadows_ && context_ &&
          REXCVAR_GET(masseffect_native_send_after_shadows)) {
        sent_after_shadows_ = true;
        context_->SendAndWait();
      }
    }
  }

  void BeforeSend() override {
    WaitUploads();  // deferred vertex copies, before flushing the mapping and submitting
    FinishPass();
    // Whatever is still on the texture bind thread, with its views, descriptors, barriers and copies, goes
    // into this upload buffer before it is closed (masseffect_native_textures_binding_thread). It is the last thing
    // recorded: after the pass closes (which may emit the deferred sky), and meanwhile the thread has kept
    // binding.
    CollectBindings(true);
    if (used_upload_ && !coherent_upload_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, upload_memory_, upload_type_, 0,
                                                    upload_real_size_, used_upload_);
    }
    if (shared_separate_ && shared_used_ && !shared_coherent_) {
      rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, shared_memory_, shared_type_, 0,
                                                    shared_real_size_, shared_used_);
    }
  }

  void UseSlot(uint32_t slot) override {
    WaitUploads();  // already empty after BeforeSend; just in case, before switching buffers
    ReportCopies();  // every 10 s, who made the vertex copies and how long the ring waited
    ReportCacheFetch();  // every 10 s, the per-fetch sampler table
    // A texture may arrive here with its bind in flight (it was prepared before this submission's first
    // Record) and that is normal; what it cannot have is its data already in the upload buffer just
    // submitted.
    CheckBindingsOnChangeOfBuffer();
    const BufferUpload& s = uploads_[slot % uploads_.size()];
    upload_ = s.buffer;
    upload_memory_ = s.memory;
    upload_real_size_ = s.real_size;
    upload_data_ = s.data;
    upload_address_ = s.address;
    slot_current_ = uint32_t(slot % uploads_.size());  // this slot's UBO set
    used_upload_ = 0;
    if (shared_separate_) {
      const BufferUpload& c = shared_bufs_[slot_current_];
      shared_data_ = c.data;
      shared_memory_ = c.memory;
      shared_real_size_ = c.real_size;
      shared_used_ = 0;
    }
    ++epoch_upload_;
    ++frame_;
    CloseFrameOfTheGuardOfTheSky();
    // The upload buffer is reset here, so the recorded offsets are no longer valid. This also covers
    // SendAndWait, which goes through here.
    dedupe_.NewFrame(frame_);
    sent_after_shadows_ = false;  // the post-shadow submission is once per frame
    /*
     * Diagnostic cvars are read once per frame, not per draw.
     *
     * REXCVAR_GET is not a variable read: it is FLAGS_##name##_storage_(), an out-of-line function call
     * (cvar.h:343). Four of them were being made per draw (one of them per vertex binding, not per draw),
     * and one returned a std::string that was then compared. With 2,345 draws per frame that shows, and it
     * serves no purpose: none of them changes within a frame.
     */
    diag_vertices_repeated_ = REXCVAR_GET(masseffect_native_diag_vertices_repeated);
    dedupe_active_ = REXCVAR_GET(masseffect_native_dedupe_vertices);
    vertices_base_zero_ = REXCVAR_GET(masseffect_native_vertices_base_zero) && !vertices_base_off_zero_;
    diag_stats_draw_ = REXCVAR_GET(masseffect_native_stats_per_draw_s) > 0;
    area_util_ = REXCVAR_GET(masseffect_native_pass_area_util);
    // The small per-draw savings.
    framing_cache_ = REXCVAR_GET(masseffect_native_framing_cache) && !framing_cache_off_;
    util_height_memo_active_ = REXCVAR_GET(masseffect_native_util_height_memo) && !util_height_memo_off_;
    key_fast_pass_ = REXCVAR_GET(masseffect_native_key_fast_pass) && !key_fast_off_pass_;
    cvars_per_frame_ = REXCVAR_GET(masseffect_native_cvars_per_frame);
    ps_alpha_only_frame_ = PsAlphaOnly();
    no_ps_no_color_frame_ = NoPsNoColor();
    no_vegetation_ = REXCVAR_GET(masseffect_shadows_no_vegetation);
    pipelines_direct_ = REXCVAR_GET(masseffect_native_pipelines_direct) && !pipelines_direct_off_;
    canonical_key_ = REXCVAR_GET(masseffect_native_canonical_key) && !canonical_off_key_;  // phase 0a
    {
      const bool new_value = REXCVAR_GET(masseffect_native_pipeline_between_passes);  // phase 0b
      if (new_value != pipeline_between_passes_ || !pipeline_between_recorded_passes_) {
        pipeline_between_passes_ = new_value;
        pipeline_between_recorded_passes_ = true;
        REXLOG_INFO("[native] C6 pipeline at pass start: {} (frame {})",
                    new_value ? "the binding is KEPT (dynamic state phase 0b)" : "rebound, as before",
                    frame_);
      }
    }
    // Dynamic state phases 1 and 2, once per command buffer: the whole buffer uses one mode, because a
    // pipeline with a fixed state invalidates the dynamic value of that state.
    {
      uint32_t mode = 0;
      if (eds12_available_ && !eds_off_ && REXCVAR_GET(masseffect_native_dynamic_state)) {
        mode |= kEds12;
      }
      if (eds3_available_ && !eds_off_ && REXCVAR_GET(masseffect_native_dynamic_state3)) {
        mode |= kEds3;
      }
      if (mode != eds_mode_ || !eds_recorded_mode_) {
        eds_recorded_mode_ = true;
        REXLOG_INFO("[native] C6 dynamic state: {} (frame {})", NameEdsMode(mode), frame_);
      }
      eds_mode_ = mode;
    }
    // The cheap PCF bit also changes the pipeline: once per frame.
    {
      const bool new_value = REXCVAR_GET(masseffect_native_pcf_cheap);
      if (new_value != pcf_cheap_) {
        pcf_cheap_ = new_value;
        REXLOG_INFO("[native] C2 shadow map sampling: {}",
                    new_value ? "single texel (cheap PCF)" : "3x3 half-texel pattern");
      }
    }
    // The shadow map's own depth bias, once per frame.
    {
      const int32_t pending = std::clamp(int32_t(REXCVAR_GET(masseffect_native_shadows_pending_bias)), 0, 100);
      if (pending != shadows_pending_bias_ || !shadows_recorded_bias_) {
        shadows_pending_bias_ = pending;
        shadows_recorded_bias_ = true;
        REXLOG_INFO("[native] C4 shadow map depth bias: slope {:.1f}{}",
                    double(pending) * 0.1, pending ? "" : " (off, as before)");
      }
    }
    // The 1/size bit changes the pipeline, so it is decided once per frame.
    {
      const bool new_value = REXCVAR_GET(masseffect_native_inv_tex_size);
      if (new_value != inv_tex_size_) {
        inv_tex_size_ = new_value;
        REXLOG_INFO("[native] C2 texture size: {}",
                    new_value ? "by constant" : "asked from the texture");
      }
    }
    /*
     * The early depth test also changes the pipeline (a different module), so it is decided once per
     * frame, like the PCF and the 1/size.
     */
    {
      const bool new_value = REXCVAR_GET(masseffect_native_z_early);
      if (new_value != z_early_ || !alternation_z_recorded_) {
        z_early_ = new_value;
        alternation_z_recorded_ = true;
        REXLOG_INFO("[native] C6 depth test: {} (frame {})",
                    new_value ? "BEFORE shading where Z is not written (early Z)"
                          : "after shading, as before",
                    frame_);
      }
    }
    skip_invisibles_ = REXCVAR_GET(masseffect_native_skip_invisibles);
    indices_cache_active_ = REXCVAR_GET(masseffect_native_indices_cache);
    // The deferred sky, also once per frame. If it is switched off mid-frame with one already deferred, the
    // pending one is still emitted through its usual path.
    {
      const bool new_value = REXCVAR_GET(masseffect_native_postponed_sky);
      if (new_value != postponed_sky_ || !recorded_sky_) {
        postponed_sky_ = new_value;
        recorded_sky_ = true;
        REXLOG_INFO("[native] C6 sky: {} (frame {})",
                    new_value ? "DEFERRED until after the pass's opaque draws"
                          : "in its original place (first, over the empty Z)",
                    frame_);
      }
    }
    ReportZEarly();
    ReportPipelinesDirect();
    ReportMinutiae();
    ReportBaseZero();
    ControlSet4();  // set 4 by differences in NVK (cvar and guard)
    ReportSet4();
    ControlDrawNvk();  // the draw path in NVK (measurement, improvements and guards)
    ReportDrawNvk();
    ReportCanonicalKey();  // Dynamic state, phase 0a
    ReportPipelineBetweenPasses();  // Dynamic state, phase 0b
    ReportDynamicState();  // dynamic state phases 1 and 2
    ReportBindings();  // texture bind thread, every 10 s
    ReportReuse();  // measurement only: new textures with a live one's content, every 10 s
    EvictTexturesIfDoesMissing();
    pool_textures_.PerFrame(frame_);  // grows with headroom, never exactly on demand
    ++sends_;  // "C6 UBO constants" report
    if (use_ubo_) {
      ++sends_ubo_;
    }
    // What the prewarm thread actually compiles also goes into the cache and has to be saved.
    if (const uint32_t compiled = prewarmed_compiled_.load(std::memory_order_relaxed);
        compiled != prewarmed_counted_) {
      pipelines_no_save_ += compiled - prewarmed_counted_;
      prewarmed_counted_ = compiled;
    }
    // The pipeline cache is saved after 64 new pipelines, or after a minute with any new one: on the
    // Switch, exiting does not always reach the destructor.
    if (pipelines_no_save_ &&
        (pipelines_no_save_ >= 64 ||
         std::chrono::steady_clock::now() - cache_saved_ >= std::chrono::seconds(60))) {
      SaveCachePipelines();
    }
  }

  void InvalidateTextures() override { ++generation_textures_; }

  // Only the cache entries that use a view of those images. The views stay valid (they go with their
  // image); what has to be redone is which slot each fetch constant gets.
  void InvalidateImages(VkImage a, VkImage b) override {
    std::array<std::pair<uint32_t, uint32_t>, 64> slots{};
    size_t n = 0;
    for (VkImage image : {a, b}) {
      if (image == VK_NULL_HANDLE || (image == b && a == b)) {
        continue;
      }
      const auto it = views_per_image_.find(image);
      if (it == views_per_image_.end()) {
        continue;
      }
      for (uint64_t key : it->second) {
        const auto v = views_.find(key);
        if (v != views_.end() && v->second.image == image && n < slots.size()) {
          slots[n++] = {v->second.heap, v->second.slot};
        }
      }
    }
    if (!n) {
      return;
    }
    const auto affected = [&](const CacheSampler& c) {
      for (size_t i = 0; i < n; ++i) {
        if (slots[i].first == c.heap && slots[i].second == c.slot) {
          return true;
        }
      }
      return false;
    };
    for (CacheSampler& c : cache_samplers_) {
      if (affected(c)) {
        c.frame = UINT64_MAX;
        c.valid_until = 0;
      }
    }
    for (CacheSampler& c : cache_fetch_) {
      if (affected(c)) {
        c.frame = UINT64_MAX;
        c.valid_until = 0;
      }
    }
  }

  uint64_t Drawn() const override { return drawn_; }

  // masseffect_native_textures_mb_max: with the texture cache above the limit, evict the textures unused for
  // the longest until it drops to 75 %, at most once every 60 frames.
  // It is safe without waiting for the GPU: a texture not prepared for 120 frames cannot be referenced by
  // any pending submission. The slot caches are valid for at most 32 frames after preparing, and only the
  // previous submission can still be on the GPU (Record waits for the slot before reusing it). The heap
  // slots are updated with UPDATE_AFTER_BIND, and the new generation invalidates the caches.
  static constexpr uint64_t kFramesNoUsageForDrop = 120;
  /*
   * How much GPU memory there is and how much is left.
   *
   * The cache limit was picked by eye twice (384 MB and then 192) because this data was not available.
   * With VK_EXT_memory_budget the driver reports the budget and the usage; without it, at least the heap
   * sizes are visible. It goes with the cache report, every 256 new textures.
   */
  void NoteMemoryOfTheGpu() {
    const auto* instance = vulkan_device_->vulkan_instance();
    if (!instance) {
      return;
    }
    const auto& ifn = instance->functions();
    const VkPhysicalDevice physical = vulkan_device_->physical_device();
    const bool has_budget = vulkan_device_->extensions().ext_EXT_memory_budget &&
                                 ifn.vkGetPhysicalDeviceMemoryProperties2 != nullptr;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    properties2.pNext = has_budget ? &budget : nullptr;
    if (has_budget) {
      ifn.vkGetPhysicalDeviceMemoryProperties2(physical, &properties2);
    } else {
      ifn.vkGetPhysicalDeviceMemoryProperties(physical, &properties2.memoryProperties);
    }
    const auto& mp = properties2.memoryProperties;
    std::string text;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i) {
      if (!text.empty()) {
        text += " | ";
      }
      const bool local = (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
      if (has_budget) {
        text += fmt::format("heap {}{}: {} MB used of {} MB budgeted (size {} MB)", i,
                             local ? " (GPU)" : "", budget.heapUsage[i] >> 20,
                             budget.heapBudget[i] >> 20, mp.memoryHeaps[i].size >> 20);
      } else {
        text += fmt::format("monton {}{}: {} MB", i, local ? " (GPU)" : "",
                             mp.memoryHeaps[i].size >> 20);
      }
    }
    REXLOG_INFO("[native] C3 GPU memory: {}{}; the texture cache holds {} MB of {} MB", text,
                has_budget ? "" : " (no VK_EXT_memory_budget: sizes only)", bytes_textures_ >> 20,
                textures_mb_max_);
    // Without this line there is no way to know whether the pool is working or fragmenting. A vegetation
    // counter once existed and was never printed.
    if (pool_textures_.Active()) {
      REXLOG_INFO("[native] C3 {}", pool_textures_.Summary());
    }
  }

  /*
   * Without stutters.
   *
   * The first version waited until the limit was exceeded and evicted 25 % at once. In play that is a
   * stutter, and a stutter every few minutes is no better than running out of memory every fourteen.
   *
   * Now a few are evicted per frame as soon as the cache nears the limit, and only cold textures: those
   * unused for 120 frames, i.e. two to four seconds. They are not in the working set, so they do not have
   * to be uploaded again right away and there is no thrashing. If there are not enough cold ones, nothing
   * is forced: the cache is allowed to grow, since going a little over is better than uploading and
   * evicting the same thing.
   */
  static constexpr uint32_t kDropPerFrame = 4;

  void EvictTexturesIfDoesMissing() {
    const uint64_t limit = uint64_t(std::max(textures_mb_max_, 0)) << 20;
    if (!limit) {
      return;
    }
    // Little by little, from 75 % of the limit. That is what avoids the stutter.
    if (bytes_textures_ > limit / 4 * 3) {
      DropSomeColdFew();
    }
    // Safety net: if it still goes over the limit (because almost everything is hot), one batch every 60
    // frames. With the above working, this should almost never trigger.
    if (bytes_textures_ > limit && frame_ >= attempt_eviction_ + 60) {
      attempt_eviction_ = frame_;
      DropTextures(limit / 4 * 3, kFramesNoUsageForDrop, "above the limit");
    }
  }

  // The kDropPerFrame oldest cold ones. Without sorting the whole cache: they are picked on the fly.
  void DropSomeColdFew() {
    std::array<std::pair<uint64_t, uint64_t>, kDropPerFrame> chosen{};  // (frame, key)
    uint32_t how_many = 0;
    for (const auto& [key, texture] : textures_) {
      // Nor those with a bind in flight.
      if (texture.image.image == VK_NULL_HANDLE || texture.needs_upload || texture.in_flight ||
          texture.frame == UINT64_MAX ||
          texture.frame + kFramesNoUsageForDrop >= frame_) {
        continue;
      }
      if (how_many < kDropPerFrame) {
        chosen[how_many++] = {texture.frame, key};
        continue;
      }
      // Replaces the most recent of the chosen ones, if this one is older.
      uint32_t worst = 0;
      for (uint32_t i = 1; i < how_many; ++i) {
        if (chosen[i].first > chosen[worst].first) {
          worst = i;
        }
      }
      if (texture.frame < chosen[worst].first) {
        chosen[worst] = {texture.frame, key};
      }
    }
    if (how_many == 0) {
      return;  // all hot: let it grow rather than thrash
    }
    std::unordered_set<VkImage> images;
    for (uint32_t i = 0; i < how_many; ++i) {
      auto it = textures_.find(chosen[i].second);
      if (it == textures_.end()) {
        continue;
      }
      images.insert(it->second.image.image);
      bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
    }
    DropImages(images);
    released_textures_ += images.size();
    released_little_to_little_ += images.size();
    if (frame_ >= warning_trickle_ + 600) {  // one warning every 600 frames, not one per texture
      warning_trickle_ = frame_;
      REXLOG_INFO("[native] C3: texture cache near the limit: cold textures dripping out ({} in total); "
                  "{} textures and {} MB of {} MB remain",
                  released_little_to_little_, textures_.size(), bytes_textures_ >> 20, textures_mb_max_);
    }
  }

  /*
   * The last resort, when the GPU has no more memory to give.
   *
   * Once nvMapCreate started failing even for 64 KB there was no way back: the driver ended up
   * quarantining ranges and the screen went black with the audio still playing. Before it gets to that,
   * half the texture cache is released.
   *
   * Normal eviction only touches what has been unused for 120 frames, because images are destroyed at
   * once and the GPU could still be reading them. Here that margin is not needed: it waits for the GPU to
   * finish everything in flight, and then none is in use. It costs a one-frame stutter; crashing costs
   * the game.
   */
  bool DropTexturesPerMissingOfMemory() override {
    if (textures_.empty() || bytes_textures_ == 0 || releasing_per_missing_of_memory_) {
      return false;  // the guard keeps an allocation inside the cleanup itself from re-entering here
    }
    releasing_per_missing_of_memory_ = true;
    bool something = false;
    if (context_ && context_->WaitGpuOfTheAll()) {
      something = DropTextures(bytes_textures_ / 2, 0, "OUT OF GPU MEMORY") > 0;
    }
    releasing_per_missing_of_memory_ = false;
    return something;
  }

  // Actually retires a group of images. First the views and their slots (pointed at the empty texture),
  // then the images. Used by both the gradual and the batch eviction.
  void DropImages(const std::unordered_set<VkImage>& images) {
    ++generation_textures_;
    for (auto it = views_.begin(); it != views_.end();) {
      if (!images.count(it->second.image)) {
        ++it;
        continue;
      }
      WriteImage(it->second.heap, it->second.slot, empty_[it->second.heap].view);
      heaps_[it->second.heap].free.push_back(it->second.slot);
      dfn_.vkDestroyImageView(device_, it->second.view, nullptr);
      views_per_image_.erase(it->second.image);  // all views of that image go
      it = views_.erase(it);
    }
    for (auto it = textures_.begin(); it != textures_.end();) {
      if (!images.count(it->second.image.image)) {
        ++it;
        continue;
      }
      RemoveContentTexture(it->second, it->first);  // measurement only: masseffect_native_diag_reuse
      DestroyImage(it->second.image);
      it = textures_.erase(it);
    }
  }

  // Evicts unused textures until below `goal`. Returns the bytes freed.
  uint64_t DropTextures(uint64_t goal, uint64_t age_min, const char* reason) {
    std::vector<std::pair<uint64_t, uint64_t>> candidate;  // (last frame it was prepared, key)
    for (const auto& [key, texture] : textures_) {
      // Nor those with a bind in flight (their image has no memory yet).
      if (texture.image.image != VK_NULL_HANDLE && !texture.needs_upload && !texture.in_flight &&
          texture.frame != UINT64_MAX &&
          texture.frame + age_min < frame_) {
        candidate.emplace_back(texture.frame, key);
      }
    }
    std::sort(candidate.begin(), candidate.end());
    const uint64_t before = bytes_textures_;
    std::unordered_set<VkImage> images;
    for (const auto& [last, key] : candidate) {
      if (bytes_textures_ <= goal) {
        break;
      }
      auto it = textures_.find(key);
      images.insert(it->second.image.image);
      bytes_textures_ -= std::min(bytes_textures_, it->second.bytes);
    }
    if (images.empty()) {
      return 0;
    }
    DropImages(images);
    released_textures_ += images.size();
    REXLOG_INFO("[native] C3: texture cache {} ({} MB): {} dropped, unused for more than {} frames "
                "({} MB); {} textures and {} MB remain ({} released in total)",
                reason, before >> 20, images.size(), age_min,
                (before - bytes_textures_) >> 20, textures_.size(), bytes_textures_ >> 20, released_textures_);
    return before - bytes_textures_;
  }

  // ZCULL: clears a depth image by opening a pass with loadOp = CLEAR. Needed for images created without
  // TRANSFER_DST (the only ones the driver gives a ZCULL plane), because vkCmdClearDepthStencilImage
  // requires that usage (VUID-vkCmdClearDepthStencilImage-pRanges-02660). It costs no extra GPU time: it
  // is exactly how NVK implements that command internally (nvk_cmd_clear.c, clear_image opens a pass with
  // loadOp = CLEAR).
  bool ClearDepthInPass(VkCommandBuffer commands, const ImageNative& image,
                               float depth, uint32_t stencil,
                               const VkRect2D* area = nullptr) override {
    if (commands == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE || !image.width ||
        !image.height || !me::native::IsSingleSample(uint32_t(image.sample_count))) {
      return false;
    }
    if (area && (!area->extent.width || !area->extent.height || area->offset.x < 0 ||
        area->offset.y < 0 || uint32_t(area->offset.x) + area->extent.width > image.width ||
        uint32_t(area->offset.y) + area->extent.height > image.height)) return false;
    FinishPass();  // a pass cannot be opened inside another
    uint32_t formats[5] = {0, 0, 0, 0, uint32_t(image.format)};
    const VkRenderPass pass = PassFor(formats, kLoadClear);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[4] = image.view;
    const VkFramebuffer framebuffer = FramebufferFor(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue clear{};
    clear.depthStencil = {depth, stencil};
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = area ? *area : VkRect2D{{0, 0}, {image.width, image.height}};
    start.clearValueCount = 1;
    start.pClearValues = &clear;
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdEndRenderPass(commands);
    return true;
  }

  // Preserve depth exactly: LOAD both aspects, then clear ONLY stencil. The
  // caller proves the guest operation is a full-mask constant stencil clear.
  // Unlike ClearDepthInPass this never uses a depth CLEAR load operation.
  bool ClearStencilInPass(VkCommandBuffer commands, const ImageNative& image,
                           uint32_t stencil, const VkRect2D& area) override {
    if (commands == VK_NULL_HANDLE || image.image == VK_NULL_HANDLE ||
        image.view == VK_NULL_HANDLE || !image.width || !image.height ||
        !area.extent.width || !area.extent.height || area.offset.x < 0 ||
        area.offset.y < 0 || stencil > 255u || !context_ ||
        !me::native::IsSingleSample(uint32_t(image.sample_count)) ||
        (image.format != VK_FORMAT_D16_UNORM_S8_UINT &&
         image.format != VK_FORMAT_D24_UNORM_S8_UINT &&
         image.format != VK_FORMAT_D32_SFLOAT_S8_UINT)) return false;
    const uint32_t x = uint32_t(area.offset.x), y = uint32_t(area.offset.y);
    // Subtraction after validating offsets avoids unsigned addition overflow.
    if (x >= image.width || y >= image.height ||
        area.extent.width > image.width - x ||
        area.extent.height > image.height - y) return false;
    const uint64_t generation = context_->GenerationCommands();
    FinishPass();
    // Closing a shadow pass may submit/rotate WORK. Never record into the old
    // handle supplied by the caller; it must close the pass before acquiring it.
    if (context_->GenerationCommands() != generation ||
        context_->CommandsWork() != commands) return false;
    const uint32_t formats[5] = {0, 0, 0, 0, uint32_t(image.format)};
    const VkRenderPass pass = PassFor(formats, kLoadRead);
    if (pass == VK_NULL_HANDLE) return false;
    std::array<VkImageView, 5> views{};
    views[4] = image.view;
    const VkFramebuffer framebuffer = FramebufferFor(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) return false;
    VkRenderPassBeginInfo start{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = area;
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    clear.clearValue.depthStencil.stencil = stencil;
    const VkClearRect rect{area, 0, 1};
    dfn_.vkCmdClearAttachments(commands, 1, &clear, 1, &rect);
    dfn_.vkCmdEndRenderPass(commands);
    // PassFor supplies external read/write dependencies for LOAD and subsequent
    // sampling/resolves; invalidate the draw-side bindings after this extra pass.
    NotifyGraphicsExternalState();
    return true;
  }

  // Mass Effect: depth-only twin of ClearStencilInPass (mode-4 redirected clear rectangles).
  bool ClearOnlyDepthInPass(VkCommandBuffer commands, const ImageNative& image,
                                   float depth, const VkRect2D& area) override {
    if (commands == VK_NULL_HANDLE || image.image == VK_NULL_HANDLE ||
        image.view == VK_NULL_HANDLE || !image.width || !image.height ||
        !area.extent.width || !area.extent.height || area.offset.x < 0 ||
        area.offset.y < 0 || !(depth >= 0.0f && depth <= 1.0f) || !context_ ||
        !me::native::IsSingleSample(uint32_t(image.sample_count)) ||
        (image.format != VK_FORMAT_D16_UNORM_S8_UINT &&
         image.format != VK_FORMAT_D24_UNORM_S8_UINT &&
         image.format != VK_FORMAT_D32_SFLOAT_S8_UINT)) return false;
    const uint32_t x = uint32_t(area.offset.x), y = uint32_t(area.offset.y);
    // Subtraction after validating offsets avoids unsigned addition overflow.
    if (x >= image.width || y >= image.height ||
        area.extent.width > image.width - x ||
        area.extent.height > image.height - y) return false;
    const uint64_t generation = context_->GenerationCommands();
    FinishPass();
    // Closing a shadow pass may submit/rotate WORK. Never record into the old
    // handle supplied by the caller; it must close the pass before acquiring it.
    if (context_->GenerationCommands() != generation ||
        context_->CommandsWork() != commands) return false;
    const uint32_t formats[5] = {0, 0, 0, 0, uint32_t(image.format)};
    const VkRenderPass pass = PassFor(formats, kLoadRead);
    if (pass == VK_NULL_HANDLE) return false;
    std::array<VkImageView, 5> views{};
    views[4] = image.view;
    const VkFramebuffer framebuffer = FramebufferFor(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) return false;
    VkRenderPassBeginInfo start{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = area;
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    VkClearAttachment clear{};
    clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    clear.clearValue.depthStencil.depth = depth;
    const VkClearRect rect{area, 0, 1};
    dfn_.vkCmdClearAttachments(commands, 1, &clear, 1, &rect);
    dfn_.vkCmdEndRenderPass(commands);
    // PassFor supplies external read/write dependencies for LOAD and subsequent
    // sampling/resolves; invalidate the draw-side bindings after this extra pass.
    NotifyGraphicsExternalState();
    return true;
  }

  // masseffect_native_clear_area_util. Clears only the `area` rectangle of a color image with a
  // loadOp = CLEAR pass: what NVK does internally for vkCmdClearColorImage (nvk_cmd_clear.c), over that
  // rectangle. What lies outside is not touched (Vulkan only loads and stores the renderArea).
  bool ClearColorInPass(VkCommandBuffer commands, const ImageNative& image, const VkClearColorValue& color,
                         const VkRect2D& area) override {
    if (commands == VK_NULL_HANDLE || image.view == VK_NULL_HANDLE || !area.extent.width || !area.extent.height ||
        !me::native::IsSingleSample(uint32_t(image.sample_count)) ||
        area.offset.x < 0 || area.offset.y < 0 || uint32_t(area.offset.x) + area.extent.width > image.width ||
        uint32_t(area.offset.y) + area.extent.height > image.height) {
      return false;
    }
    FinishPass();  // a pass cannot be opened inside another
    uint32_t formats[5] = {uint32_t(image.format), 0, 0, 0, 0};
    const VkRenderPass pass = PassFor(formats, kLoadClear);
    if (pass == VK_NULL_HANDLE) {
      return false;
    }
    std::array<VkImageView, 5> views{};
    views[0] = image.view;
    const VkFramebuffer framebuffer = FramebufferFor(pass, views, image.width, image.height);
    if (framebuffer == VK_NULL_HANDLE) {
      return false;
    }
    VkClearValue clear{};
    clear.color = color;
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = area;
    start.clearValueCount = 1;
    start.pClearValues = &clear;
    dfn_.vkCmdBeginRenderPass(commands, &start, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdEndRenderPass(commands);
    return true;
  }

  // masseffect_native_framebuffers_forget_views. The render target code is about to destroy this view (Destroy, with the GPU
  // idle for that image): the FramebufferFor framebuffers that use it are destroyed and removed from the
  // cache. It walks the created framebuffers (dozens); never during a draw.
  // Self-checking guard: the cache and its view list must stay in step (only FramebufferFor inserts) and
  // no pass may be open (whoever destroys the view has already submitted the work). Otherwise: MISMATCH
  // in the log and, for the rest of the session, nothing is destroyed at runtime: the affected
  // framebuffers (or the whole cache, if they are out of step) are retired and destroyed at shutdown.
  // Retiring is never worse than the previous behaviour.
  void ForgetView(VkImageView view) override {
    if (view == VK_NULL_HANDLE || !REXCVAR_GET(masseffect_native_framebuffers_forget_views)) {
      return;
    }
    const bool in_parallel = framebuffers_views_.size() == framebuffers_.size();
    if (!fb_retire_ && (active_pass_ || !in_parallel)) {
      fb_retire_ = true;
      REXLOG_ERROR("[native] C6 framebuffers: MISMATCH when destroying view {:016X}: {}. For the rest "
                   "of the session the affected framebuffers are retired without being destroyed (they are destroyed on shutdown)",
                   uint64_t(reinterpret_cast<uintptr_t>(view)),
                   active_pass_ ? "a pass is open (whoever destroys the view has not submitted the work)"
                                : "the cache and its view list are out of step (someone creates framebuffers by another "
                                  "path)");
    }
    uint32_t outside = 0;
    if (!in_parallel) {
      // Unknown which ones use the view: drop them all (they are rebuilt on request).
      for (auto& [key, framebuffer] : framebuffers_) {
        fb_retired_.push_back(framebuffer);
      }
      outside = uint32_t(framebuffers_.size());
      framebuffers_.clear();
      framebuffers_views_.clear();
    } else {
      for (auto it = framebuffers_views_.begin(); it != framebuffers_views_.end();) {
        if (std::find(it->second.begin(), it->second.end(), view) == it->second.end()) {
          ++it;
          continue;
        }
        const auto fb = framebuffers_.find(it->first);
        if (fb != framebuffers_.end()) {
          if (fb_retire_) {
            fb_retired_.push_back(fb->second);
          } else {
            dfn_.vkDestroyFramebuffer(device_, fb->second, nullptr);
          }
          framebuffers_.erase(fb);
          ++outside;
        }
        it = framebuffers_views_.erase(it);
      }
    }
    ++fb_forgotten_views_;
    fb_forgotten_ += outside;
    if (fb_forgotten_views_ <= 16 || (fb_forgotten_views_ & 63) == 0) {
      MASSEFFECT_REPORT_RING("[native] C6 framebuffers: view {:016X} destroyed: {} framebuffers {} with "
                           "it; {} remain in the cache ({} views and {} framebuffers since start-up; {} "
                           "retired)",
                           uint64_t(reinterpret_cast<uintptr_t>(view)), outside,
                           fb_retire_ ? "retired (not destroyed)" : "destroyed", framebuffers_.size(),
                           fb_forgotten_views_, fb_forgotten_, fb_retired_.size());
    }
  }

  void ForgetImage(VkImage image) override {
    ++generation_textures_;  // the sampler cache could point to a retired view
    const auto index = views_per_image_.find(image);  // without walking views_
    if (index == views_per_image_.end()) {
      return;
    }
    for (uint64_t key : index->second) {
      const auto it = views_.find(key);
      if (it == views_.end() || it->second.image != image) {
        continue;
      }
      WriteImage(it->second.heap, it->second.slot, empty_[it->second.heap].view);
      heaps_[it->second.heap].free.push_back(it->second.slot);
      dfn_.vkDestroyImageView(device_, it->second.view, nullptr);
      views_.erase(it);
    }
    views_per_image_.erase(index);
  }

  StatsDraws Stats() const override {
    StatsDraws e;
    e.drawn = drawn_;
    e.rejected = rejected_;
    e.pipelines = pipelines_.size();
    e.textures = textures_.size();
    e.uploads_texture = uploads_texture_;
    e.uploaded_megabytes = megabytes_bytes_ >> 20;
    e.megabytes_textures = bytes_textures_ >> 20;
    e.ms_pipelines = ns_pipelines_ / 1000000;
    e.passes = started_passes_;
    e.full_sends = full_sends_;
    e.ns_full_sends = ns_full_sends_;
    e.bytes_vertices = bytes_vertices_;
    e.dedupe_hits = dedupe_.hits();
    e.dedupe_bytes = dedupe_.bytes_saved();
    e.dedupe_collisions = dedupe_.collisions();
    e.bytes_indices = bytes_indices_uploaded_;
    e.samplers = samplers_prepared_;
    e.samplers_cache = samplers_cache_ + samplers_cache_fetch_;
    e.ns_passes = ns_passes_;
    e.ns_vertices = ns_vertices_;
    e.computed_inputs = computed_inputs_;
    e.ns_inputs = ns_inputs_;
    e.reused_inputs = reused_inputs_ + inputs_cache_hits_;
    e.passes_per_generation = passes_per_generation_;
    e.passes_per_target = passes_per_target_;
    e.resumed_passes = resumed_passes_;
    e.ns_render_pass = ns_render_pass_;
    e.texels_passes = texels_passes_;
    e.texels_per_category = texels_per_category_;
    e.draws_per_category = draws_per_category_;
    e.draws_ps_useless = draws_ps_useless_;
    e.draws_ps_required = draws_ps_required_;
    e.shadows_active_alpha = shadows_active_alpha_;
    e.shadows_off_alpha = shadows_off_alpha_;
    e.triangles_per_category = triangles_per_category_;
    e.passes_per_category = passes_per_category_;
    e.sends = sends_;
    e.sends_ubo = sends_ubo_;
    e.shared_looked = shared_looked_;
    e.shared_changed = shared_changed_;
    e.bytes_repeated_frame = bytes_repeated_frame_;
    e.bytes_equal_previous = bytes_equal_previous_;
    e.ns_hash_vertices = ns_hash_vertices_;
    e.causes.assign(causes_.begin(), causes_.end());
    e.stages_ns = stages_ns_;
    e.scene_with_discard = scene_with_discard_;
    e.scene_no_discard = scene_no_discard_;
    e.drawn_timed = drawn_timed_;
    e.vegetation_soon = draws_vegetation_soon_;
    std::sort(e.causes.begin(), e.causes.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return e;
  }

 private:
  bool Reject(uint32_t cause, const char* text) {
    ++rejected_;
    ++causes_[cause];
    last_cause_ = cause;
    Warn(cause, text);
    return false;
  }

  void Warn(uint32_t cause, const char* text) {
    if (warned_.insert(cause).second) {
      REXLOG_WARN("[native] C6: {} (cause {})", text, cause);
    }
  }

  // --- Texture binds on a separate thread (masseffect_native_textures_binding_thread) ----------------------------------
  /*
   * Why. With the memory pool, creating a texture no longer asks the system for memory, but vkBindImageMemory
   * still makes two ioctls: reserving the plane's VA with its pte_kind and mapping the pool chunk into it
   * (nvk_image_plane_bind, nvk_image.c:1696-1710; vkCreateImage reserves no VA except for sparse images,
   * :1337-1350). All our textures are tiled with pte_kind GENERIC_16BX2 (nil/image.rs:439-440 and 876-918),
   * so both are always paid. In play: 336 VA reservations in 95 ms and 336 mappings in 111 ms, ~0.6 ms of
   * wall time per texture with the ring asleep in the ioctl. Entering a new zone brings 33-45 textures in
   * one frame: 19-27 ms of stalled ring that did not show up in "textures X ms".
   *
   * How. The ring does the CPU part (vkCreateImage, requirements, pool chunk, descriptor slot) and only
   * queues the vkBindImageMemory. The draw is recorded with its slot and UploadTexture leaves the data in the
   * upload buffer as usual. In BeforeSend (CollectBindings) it waits for whatever is missing, creates the
   * views, writes the descriptors (UPDATE_AFTER_BIND: legal until submission) and records the barrier and
   * the copy in that same upload buffer, which goes in the same vkQueueSubmit, ahead of the work. The GPU
   * receives exactly the same thing in the same submission.
   *
   * The driver. vkBindImageMemory from another thread is safe: the VA and the mapping are under
   * device->va_mutex (horizon/nouveau_horizon_vm.c:173 and 377), the mapping refcount under
   * memory_identity_mutex (nouveau_horizon_memory.c:1065), and everything else belongs to that image. The
   * pool is not touched from the thread: it stays single-threaded (masseffect_native_textures_pool.h).
   *
   * Threads. Shared state is under bindings_mutex_, and every decision
   * to sleep or wake is taken with the lock held; the thread only reads the image, memory and offset of its
   * request and stores the result under the lock. The ring's wait has a timeout and logs every 2 s.
   * Persistent thread (never detached), joined in the destructor before any image is destroyed. Priority
   * 0x2D: a host service, above the guest.
   *
   * Self-checking guard. Observing phase: the first kBindingsToCheck wait for their bind right after
   * queuing it (same order as before: only the thread doing the ioctl changes). If all succeed, applying
   * phase. In any phase, a failed bind, a wait of more than 2 s or a deferred copy that can no longer go in
   * its upload buffer: REXLOG_ERROR with the MISMATCH and off for the session (the usual path returns;
   * whatever was in flight finishes through the fallback path). And if for 3 reports in a row the ring
   * waits for the thread longer than the thread takes to bind, it does not pay off: it switches off too.
   */
  void DecideBindings() {
    const bool request = REXCVAR_GET(masseffect_native_textures_binding_thread);
    bindings_priority_ = std::clamp<int32_t>(REXCVAR_GET(masseffect_native_textures_binding_thread_priority), 0x2C, 0x3B);
    bindings_phase_ = request ? kBindingsLooking : kOffBindings;
    REXLOG_INFO("[native] C3: binding of new textures on a separate thread (masseffect_native_textures_binding_thread) = {}",
                request ? fmt::format("YES, priority {:#x}; LOOKING phase: the first {} wait for their binding immediately "
                                     "(pool textures only, which is {})",
                                     bindings_priority_, kBindingsToCheck,
                                     pool_textures_.Active() ? "on" : "OFF: it will not be used")
                       : std::string("no, on the ring thread as usual"));
  }

  void TurnOffBindings(const std::string& reason) {
    if (bindings_phase_ == kOffBindings) {
      return;
    }
    bindings_phase_ = kOffBindings;
    REXLOG_ERROR("[native] C3: texture-binding thread SWITCHED OFF for the rest of the session: {}. New textures "
                 "are created in full on the ring thread again ({} bound on the thread, {} failed, {} views and {} "
                 "deferred copies)",
                 reason, bindings_thread_total_, failed_bindings_, postponed_views_, postponed_copies_);
  }

  // Queues the vkBindImageMemory of an image. false = not queued (queue full or no thread): the usual path.
  bool EnqueueBinding(VkImage image, VkDeviceMemory memory, VkDeviceSize offset, uint64_t& ticket) {
    if (!bindings_thread_.joinable()) {
      try {
        bindings_thread_ = std::thread([this] { LoopBindings(); });
      } catch (const std::system_error& error) {
        TurnOffBindings(fmt::format("MISMATCH: could not create the thread ({})", error.what()));
        return false;
      }
    }
    bool warn = false;
    {
      std::lock_guard<std::mutex> latch(bindings_mutex_);
      if (bindings_requests_ - collected_bindings_ >= kQueueBindings) {
        ++bindings_full_queue_;
        return false;
      }
      SubmissionBinding& submission = queue_bindings_[bindings_requests_ & (kQueueBindings - 1)];
      submission.image = image;
      submission.memory = memory;
      submission.offset = offset;
      submission.result = VK_NOT_READY;
      submission.ns = 0;
      ticket = bindings_requests_++;
      warn = bindings_sleeping_;  // decided under the lock: if it sleeps, it is inside its wait with the predicate
    }
    if (warn) {
      bindings_cv_.notify_one();
    }
    return true;
  }

  // The thread: the requests' vkBindImageMemory calls, in order. It sleeps on a decision taken under the
  // lock; it never spins. It does not write to the log (the ring records the figures in ReportBindings).
  void LoopBindings() {
    rex::thread::set_current_thread_name("MASSEFFECT texture bindings");
    const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(bindings_priority_));
    std::unique_lock<std::mutex> latch(bindings_mutex_);
    bindings_priority_ok_ = priority_ok;
    for (;;) {
      if (done_bindings_ == bindings_requests_) {
        if (bindings_stop_) {
          return;  // stop, nothing pending
        }
        if (bindings_waiting_) {
          done_bindings_cv_.notify_one();
        }
        bindings_sleeping_ = true;
        bindings_cv_.wait(latch, [this] { return bindings_stop_ || done_bindings_ != bindings_requests_; });
        bindings_sleeping_ = false;
        continue;
      }
      // The slot is not reused until the ring collects it (collected_bindings_): it stays ours without the lock.
      SubmissionBinding& submission = queue_bindings_[done_bindings_ & (kQueueBindings - 1)];
      const VkImage image = submission.image;
      const VkDeviceMemory memory = submission.memory;
      const VkDeviceSize offset = submission.offset;
      latch.unlock();
      const auto before = std::chrono::steady_clock::now();
      const VkResult result = dfn_.vkBindImageMemory(device_, image, memory, offset);
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - before)
                                       .count());
      latch.lock();
      submission.result = result;
      submission.ns = ns;
      ns_bindings_thread_ += ns;
      ns_binding_worst_ = std::max(ns_binding_worst_, ns);
      ++done_bindings_;
      if (bindings_waiting_) {
        done_bindings_cv_.notify_one();
      }
    }
  }

  void StopBindings() {
    if (!bindings_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> latch(bindings_mutex_);
      bindings_stop_ = true;
    }
    bindings_cv_.notify_one();
    bindings_thread_.join();  // first finishes every queued request
    REXLOG_INFO("[native] C3: texture-binding thread stopped ({} bound on the thread, {} failed, {} views and {} "
                "deferred copies, {} slots lost)",
                bindings_thread_total_, failed_bindings_, postponed_views_, postponed_copies_, lost_slots_);
  }

  // The ring waits until the thread has done the requests before `goal`. With a timeout and a log line
  // every 2 s.
  void WaitBindings(uint64_t goal) {
    std::unique_lock<std::mutex> latch(bindings_mutex_);
    if (done_bindings_ >= goal) {
      return;
    }
    const auto before = std::chrono::steady_clock::now();
    bindings_waiting_ = true;
    if (bindings_sleeping_) {
      bindings_cv_.notify_one();  // should not be needed (it has work); in case a wake-up was lost
    }
    int seconds = 0;
    while (!done_bindings_cv_.wait_for(latch, std::chrono::seconds(2),
                                        [this, goal] { return done_bindings_ >= goal; })) {
      seconds += 2;
      stuck_bindings_ = true;
      REXLOG_ERROR("[native] C3: the ring thread has been waiting {} s for texture bindings: goal {}, done "
                   "{}, requested {}; the thread is {}",
                   seconds, goal, done_bindings_, bindings_requests_,
                   bindings_sleeping_ ? "ASLEEP (lost wake-up)" : "awake (an ioctl that does not return)");
    }
    bindings_waiting_ = false;
    latch.unlock();
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - before)
                                     .count());
    ns_wait_bindings_total_ += ns;
    ns_wait_bindings_report_ += ns;
    ns_wait_bindings_worst_ = std::max(ns_wait_bindings_worst_, ns);
    ++waits_bindings_report_;
    masseffect::waits::g_ns_waiting_bindings.fetch_add(ns, std::memory_order_relaxed);
  }

  /*
   * The CPU part of CreateTexture (image, requirements, pool chunk) here, and its vkBindImageMemory to the
   * thread. false = nothing was touched and the caller continues through CreateTexture: thread off, pool off
   * or full, queue full, or (in the observing phase) a failed bind whose fallback path also failed.
   */
  bool CreateTextureInThread(Texture& texture, VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                          uint32_t background, uint32_t levels) {
    if (bindings_phase_ == kBindingsNoDecide) {
      DecideBindings();
    }
    if (bindings_phase_ == kOffBindings || !pool_textures_.Active()) {
      return false;
    }
    const VkImageCreateInfo info = InfoImageTexture(format, width, height, layers, background, levels);
    VkImage image = VK_NULL_HANDLE;
    if (dfn_.vkCreateImage(device_, &info, nullptr, &image) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    dfn_.vkGetImageMemoryRequirements(device_, image, &req);
    VkDeviceMemory block = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t id_block = 0xFFFFFFFFu;
    if (!pool_textures_.Reserve(req, block, offset, id_block)) {
      dfn_.vkDestroyImage(device_, image, nullptr);
      return false;  // no room in the pool: CreateTexture, which can take the dedicated path
    }
    uint64_t ticket = 0;
    if (!EnqueueBinding(image, block, offset, ticket)) {
      dfn_.vkDestroyImage(device_, image, nullptr);  // before the chunk, as the pool requires (not bound yet)
      pool_textures_.Release(id_block);
      return false;
    }
    texture.image.image = image;
    texture.image.memory = VK_NULL_HANDLE;  // from the pool: not freed on its own
    texture.image.pool_block = id_block;
    texture.image.width = width;
    texture.image.height = height;
    texture.image.format = format;
    texture.image.prepared = false;
    TextureInFlight flight;
    flight.texture = &texture;
    flight.ticket = ticket;
    flight.image = image;
    flight.format = format;
    flight.width = width;
    flight.height = height;
    flight.layers = layers;
    flight.background = background;
    flight.levels = levels;
    in_flight_.push_back(flight);
    texture.in_flight = uint32_t(in_flight_.size());
    images_in_flight_.insert(image);
    if (bindings_phase_ == kBindingsLooking) {
      // Observing phase: wait now, before SlotView and UploadTexture: everything stays as without the thread.
      CollectBindings(true);
      if (texture.image.image == VK_NULL_HANDLE) {
        return false;  // neither the thread nor the fallback (already logged): CreateTexture, with its emergency path
      }
      if (++checked_bindings_ >= kBindingsToCheck && bindings_phase_ == kBindingsLooking) {
        bindings_phase_ = kBindingsApplying;
        uint64_t ns_thread = 0;
        bool priority_ok = true;
        {
          std::lock_guard<std::mutex> latch(bindings_mutex_);
          ns_thread = ns_bindings_thread_;
          priority_ok = bindings_priority_ok_;
        }
        REXLOG_INFO("[native] C3: texture-binding thread: {} bindings with immediate wait and none failed ({:.0f} "
                    "us average inside vkBindImageMemory; the ring waited {:.0f} us on average per texture). Moves to the "
                    "APPLYING phase: the wait goes before each submission{}",
                    checked_bindings_, double(ns_thread) / 1e3 / double(checked_bindings_),
                    double(ns_wait_bindings_total_) / 1e3 / double(checked_bindings_),
                    priority_ok ? "" : " (the kernel did NOT accept the thread priority)");
      }
    }
    return true;
  }

  // SlotView for an image with its bind in flight: the slot now, the view in CollectBindings.
  uint32_t SlotViewInFlight(uint64_t key, VkImage image, VkFormat format, uint32_t swizzle,
                              uint16_t swizzle_host, uint32_t heap) {
    View view;
    view.image = image;
    view.heap = heap;
    view.slot = ReserveSlot(heap);
    if (!view.slot) {
      Warn(35, "texture pool full");
      return 0;
    }
    views_.emplace(key, view);  // vista.vista stays VK_NULL_HANDLE until CollectBindings
    views_per_image_[image].push_back(key);
    ViewInFlight postponed;
    postponed.key = key;
    postponed.image = image;
    postponed.format = format;
    postponed.swizzle = swizzle;
    postponed.swizzle_host = swizzle_host;
    postponed.heap = heap;
    postponed.slot = view.slot;
    views_in_flight_.push_back(postponed);
    ++postponed_views_;
    ++postponed_views_report_;
    return view.slot;
  }

  // UploadTexture for a texture with its bind in flight: the data is already in the upload buffer; record
  // where.
  bool PostponeCopy(Texture& texture, VkDeviceSize offset, VkCommandBuffer upload) {
    const size_t index = size_t(texture.in_flight) - 1;
    if (index >= in_flight_.size() || in_flight_[index].texture != &texture || in_flight_[index].copy) {
      TurnOffBindings(fmt::format("MISMATCH: inconsistent in-flight texture index ({} of {})", index,
                                in_flight_.size()));
      return false;
    }
    TextureInFlight& flight = in_flight_[index];
    flight.copy = true;
    flight.copy_offset = offset;
    flight.copy_epoch = epoch_upload_;
    flight.copy_commands = upload;
    ++postponed_copies_;
    ++postponed_copies_report_;
    return true;
  }

  // The deferred views of an image that had to be recreated: new key (it goes with the image) and its list.
  void MoveViewsInFlight(VkImage old, VkImage new_entry) {
    if (old == new_entry) {
      return;
    }
    std::vector<uint64_t> keys;
    for (ViewInFlight& postponed : views_in_flight_) {
      if (postponed.image != old) {
        continue;
      }
      const auto it = views_.find(postponed.key);
      if (it == views_.end()) {
        continue;
      }
      View view = it->second;
      views_.erase(it);
      view.image = new_entry;
      postponed.image = new_entry;
      if (new_entry != VK_NULL_HANDLE) {
        postponed.key = ViewKey(new_entry, postponed.swizzle, postponed.swizzle_host, postponed.heap);
      }
      if (!views_.emplace(postponed.key, view).second) {
        postponed.key = 0;  // key collision: CollectBindings puts the empty one in its slot
        continue;
      }
      keys.push_back(postponed.key);
    }
    views_per_image_.erase(old);  // an image in flight only has deferred views
    if (new_entry != VK_NULL_HANDLE && !keys.empty()) {
      auto& list = views_per_image_[new_entry];
      list.insert(list.end(), keys.begin(), keys.end());
    }
  }

  /*
   * Finishes everything the bind thread has. Ring thread only. `with_copies` = false outside BeforeSend
   * (the upload buffer would no longer be the one holding that data): those textures are uploaded again at
   * their next check.
   */
  void CollectBindings(bool with_copies) {
    // No re-entry: if something in here ended up starting new work (UseSlot), that call does nothing and
    // the outer one sees the upload buffer changed (MISMATCH and a full upload at the next check).
    if (in_flight_.empty() || collecting_bindings_) {
      return;
    }
    collecting_bindings_ = true;
    const auto start = std::chrono::steady_clock::now();
    const uint64_t wait_before = ns_wait_bindings_total_;
    const uint64_t goal = in_flight_.back().ticket + 1;
    WaitBindings(goal);
    if (stuck_bindings_) {
      stuck_bindings_ = false;
      TurnOffBindings("MISMATCH: a wait of more than 2 s for the thread (see the lines above)");
    }
    {
      std::lock_guard<std::mutex> latch(bindings_mutex_);
      for (TextureInFlight& flight : in_flight_) {
        flight.result = queue_bindings_[flight.ticket & (kQueueBindings - 1)].result;
      }
      collected_bindings_ = goal;  // from here on its slots can be reused
    }
    // 1. Results, in order. If a bind failed, that image is dropped (no command references it yet) and the
    //    texture is created again through CreateTexture without the emergency path: releasing half the cache
    //    submits the work, and we are before that.
    for (TextureInFlight& flight : in_flight_) {
      Texture& texture = *flight.texture;
      images_in_flight_.erase(flight.image);
      texture.in_flight = 0;
      if (flight.result == VK_SUCCESS) {
        ++bindings_thread_total_;
        ++bindings_thread_report_;
        masseffect::waits::g_bound_textures_thread.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      ++failed_bindings_;
      TurnOffBindings(fmt::format("MISMATCH: vkBindImageMemory returned {} on the thread for a {}x{} texture ({} "
                                "levels, {} layers)",
                                int32_t(flight.result), flight.width, flight.height, flight.levels, flight.layers));
      dfn_.vkDestroyImage(device_, flight.image, nullptr);
      pool_textures_.Release(texture.image.pool_block);
      texture.image.image = VK_NULL_HANDLE;
      texture.image.pool_block = 0xFFFFFFFFu;
      if (!CreateTexture(texture.image, flight.format, flight.width, flight.height, flight.layers, flight.background,
                        flight.levels, false)) {
        // Not even then. The texture is left without an image: its draws in this submission see the empty one
        // (the same as when it cannot be created) and the next time it is used it is created in full, with the
        // emergency path.
        texture.image = ImageNative{};
        bytes_textures_ -= std::min(bytes_textures_, texture.bytes);
        texture.bytes = 0;
      }
      MoveViewsInFlight(flight.image, texture.image.image);
    }
    // 2. Deferred views: created and written to their slot. If that is not possible, the slot keeps the
    //    empty one (what a draw sees when SlotView returns 0) and is not reused: this submission
    //    references it (it is counted).
    for (const ViewInFlight& postponed : views_in_flight_) {
      const auto it = postponed.key ? views_.find(postponed.key) : views_.end();
      if (it != views_.end() && it->second.image != VK_NULL_HANDLE &&
          CreateViewTexture(it->second.image, postponed.format, postponed.swizzle, postponed.swizzle_host,
                            postponed.heap, it->second.view) == VK_SUCCESS) {
        WriteImage(postponed.heap, postponed.slot, it->second.view);
        continue;
      }
      WriteImage(postponed.heap, postponed.slot, empty_[postponed.heap].view);
      if (it != views_.end()) {
        views_.erase(it);  // the image has no views_per_image_ left (MoveViewsInFlight) or is rebuilt when used
      }
      ++lost_slots_;
      Warn(34, "could not create a texture view");
    }
    views_in_flight_.clear();
    // 3. Deferred barriers and copies, in the upload buffer where UploadTexture left their data.
    VkCommandBuffer upload = VK_NULL_HANDLE;
    for (const TextureInFlight& flight : in_flight_) {
      if (!flight.copy) {
        continue;
      }
      Texture& texture = *flight.texture;
      if (texture.image.image == VK_NULL_HANDLE) {
        continue;  // no image: created and fully uploaded next time it is used
      }
      if (with_copies && upload == VK_NULL_HANDLE) {
        upload = context_->CommandsUpload();
      }
      if (!with_copies || upload == VK_NULL_HANDLE || upload != flight.copy_commands ||
          flight.copy_epoch != epoch_upload_) {
        TurnOffBindings(fmt::format("MISMATCH: a deferred copy can no longer go into its upload buffer (epoch {} "
                                  "versus {}, {})",
                                  flight.copy_epoch, epoch_upload_,
                                  with_copies ? "another upload buffer" : "collected on buffer change"));
        texture.image.prepared = false;  // the next check uploads it again in full, with its barrier
        texture.next = 0;
        continue;
      }
      if (!texture.image.prepared) {
        Barrier(upload, texture.image.image, texture.layers);
        texture.image.prepared = true;
      }
      RecordCopyTexture(upload, texture, flight.copy_offset);
    }
    in_flight_.clear();
    collecting_bindings_ = false;
    if (!measuring_creation_) {  // inside PrepareTexture its stopwatch already counts it
      const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count());
      const uint64_t wait = ns_wait_bindings_total_ - wait_before;
      const uint64_t own = ns > wait ? ns - wait : 0;
      masseffect::waits::g_ns_create_textures.fetch_add(own, std::memory_order_relaxed);
      ns_create_report_ += own;
    }
  }

  // UseSlot: a texture with its bind in flight may reach new work (it was prepared before that work's
  // first Record), but not with its data already in the previous upload buffer: that should never happen.
  void CheckBindingsOnChangeOfBuffer() {
    for (const TextureInFlight& flight : in_flight_) {
      if (flight.copy) {
        CollectBindings(false);  // logs the MISMATCH, switches off and leaves those textures to be uploaded again
        return;
      }
    }
  }

  // Every 10 s, if there were new textures: what the thread did and how long the ring waited. This is also
  // where it is decided whether it pays off: if for 3 reports in a row the ring waits for the thread longer
  // than the thread takes to bind, the ring would do better doing it itself, and it switches off.
  void ReportBindings() {
    if (bindings_phase_ == kBindingsNoDecide) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_bindings_ < std::chrono::seconds(10)) {
      return;
    }
    report_bindings_ = now;
    uint64_t ns_thread = 0;
    uint64_t worst_thread = 0;
    uint64_t full_queue = 0;
    bool priority_ok = true;
    {
      std::lock_guard<std::mutex> latch(bindings_mutex_);
      ns_thread = ns_bindings_thread_ - ns_bindings_previous_thread_;
      ns_bindings_previous_thread_ = ns_bindings_thread_;
      worst_thread = ns_binding_worst_;
      ns_binding_worst_ = 0;
      full_queue = bindings_full_queue_ - full_previous_queue_;
      full_previous_queue_ = bindings_full_queue_;
      priority_ok = bindings_priority_ok_;
    }
    const uint64_t created = created_textures_ - created_previous_report_;
    created_previous_report_ = created_textures_;
    if (created || bindings_thread_report_ || waits_bindings_report_) {
      static constexpr const char* kPhases[] = {"off", "LOOKING", "APPLYING"};
      MASSEFFECT_REPORT_RING(
          "[native] C3 new textures, last 10 s: {} created, {} with the binding on the thread; the ring "
          "spent {:.1f} ms creating and waited for the thread {:.1f} ms in {} waits (worst {:.2f} ms); the thread, {:.1f} ms inside "
          "vkBindImageMemory (average {:.0f} us, worst {:.2f} ms); {} views and {} deferred copies; {} through the usual "
          "path with a full queue; phase {}{}",
          created, bindings_thread_report_, double(ns_create_report_) / 1e6, double(ns_wait_bindings_report_) / 1e6,
          waits_bindings_report_, double(ns_wait_bindings_worst_) / 1e6, double(ns_thread) / 1e6,
          bindings_thread_report_ ? double(ns_thread) / 1e3 / double(bindings_thread_report_) : 0.0,
          double(worst_thread) / 1e6, postponed_views_report_, postponed_copies_report_, full_queue,
          kPhases[std::clamp<int32_t>(bindings_phase_, 0, 2)], priority_ok ? "" : " (the kernel did NOT accept the priority)");
    }
    if (bindings_phase_ == kBindingsApplying && bindings_thread_report_ >= 16) {
      reports_no_compensate_ = ns_wait_bindings_report_ > ns_thread ? reports_no_compensate_ + 1 : 0;
      if (reports_no_compensate_ >= 3) {
        TurnOffBindings(fmt::format("not worth it: in 3 consecutive reports the ring waited for the thread longer than the thread "
                                  "took to bind (the last one, {:.1f} ms of waiting for {:.1f} ms of binds)",
                                  double(ns_wait_bindings_report_) / 1e6, double(ns_thread) / 1e6));
      }
    }
    bindings_thread_report_ = 0;
    waits_bindings_report_ = 0;
    ns_wait_bindings_report_ = 0;
    ns_wait_bindings_worst_ = 0;
    ns_create_report_ = 0;
    postponed_views_report_ = 0;
    postponed_copies_report_ = 0;
  }

  // --- Vertex copies on a separate thread (masseffect_native_uploads_thread) ------------------------------------------
  // One producer (the ring thread, in Draw) and one consumer (copies_thread_). The producer writes the job
  // into written_copies_ and publishes it; the thread copies up to there and publishes done_copies_.
  // With the queue full the copy is done on the ring, as before. The destinations are distinct ranges of
  // the upload buffer, so order does not matter. The copies must be waited for before submitting the work
  // (BeforeSend) and before returning the read pointer to the game (WaitUploads from
  // the native graphics system): after that the game may reuse its vertex buffers.
  //
  // Sleeping and waking are decided under the lock. Two earlier versions hung: the copy thread asleep with
  // 1,123 copies queued and the ring waiting for them. Each side used to decide without the lock whether to
  // wake the other, Dekker-style: the ring published written_copies_ and read done_copies_, and the
  // thread the other way round. With store(..., memory_order_release) MSVC's STL emits a plain mov, and on
  // x86 (TSO) a store may not yet be visible from another core: both sides missed the wake-up. ARM gives no
  // guarantee either. Now copies_sleeping_ and copies_waiting_ are only touched under the lock, and
  // nothing is decided without it: the wake-up arrives at the next point that takes the lock (every
  // kCopiesPerWarning copies, when the ring waits, and when the thread runs out of work). There is no atomic
  // hint flag any more; the thread always signals from its no-work path, under the lock, which it goes
  // through as soon as it finishes.
  // Only the queuing thread may wait (single producer). Otherwise it wakes the thread once and waits
  // without helping.
  //
  /*
   * The ring helps instead of waiting (masseffect_native_uploads_help).
   *
   * Why. In play the ring waited 14.7 ms for the copy thread during one stutter (3,965 draws, 5 waits)
   * and 17.9 ms in the next (3,890 draws, 3 waits; the game spent 21.1 ms without room in the ring). Copying
   * what those frames needed is ~9-11 MB, ~6-7 ms at the rate the profile gives the thread (1.5 GB/s): the
   * rest was the thread ready and without a core. It ran at 0x3B, below the two game threads (0x3A), and
   * Horizon only moves a ready thread to another core when that core runs out of work: with the CPU at
   * 278 % out of 300 (the profile block for that stutter), almost never. And the ring (0x2D) waited for it
   * on a condition variable, which does not lend priority: a textbook priority inversion.
   *
   * How. On every wait with pending copies (by the queuing thread, in the applying phase):
   *   1. the ring takes and copies, in batches of kCopiesPerChunk, whatever the thread has not taken yet.
   *      If the thread is running on another core they share what is left; if it has no core, the ring
   *      does it all, without waiting for it.
   *   2. whatever is missing can only be the batch the thread is working on. The ring waits by taking
   *      copies_chunk_mutex_, which the thread holds while copying: a libnx mutex waits in the kernel
   *      (svcArbitrateLock) and Horizon lends the owner the waiter's priority, so the thread rises to 0x2D
   *      until it releases it. It is one batch.
   * The thread's own priority is not changed: it takes no core from anyone while the ring is not waiting
   * for it.
   *
   * Counters. written_copies_ (ring only), taken_copies_ (the thread or the ring, with CAS, in order)
   * and done_copies_ (how many are done, whoever did them): done <= taken <= written. Outside
   * WaitUploads only the thread takes, so it finishes in order and "done = h" still means the first h
   * are done: EnqueueCopy's space accounting still holds. Inside, the ring does not queue, and it leaves
   * with everything done.
   *
   * Same data. Each copy is done exactly once by CopyVertices with the same slot (source, destination,
   * words and byte order), and all of them before returning from WaitUploads, as before: before
   * submitting and before returning the read pointer. Only who does it changes, and sometimes it is done
   * earlier.
   *
   * Self-checking guard. Each copy leaves its index + 1 in copies_marks_. On each wait the ring checks the
   * marks of what was queued since the previous one and that the counters add up; a copy without a mark is
   * done right there, before submitting and before returning the read pointer, and it is a MISMATCH
   * (REXLOG_ERROR). Phases: observing (the first kWaitsCopiesLooking waits with pending copies: the
   * thread already takes batches, the ring waits as before and checks), applying (help) and, after a
   * MISMATCH with help on, no help (the plain wait). A MISMATCH without help, or with the counters out
   * of step, turns the thread off for the session and the copies go back to the ring (the original
   * behaviour). The plain wait no longer waits forever: after 2 s it takes the batch mutex and finishes the
   * job. The batch mutex has no timeout because inside it the thread only takes and copies.
   */
  enum PhaseCopies : int32_t { kCopiesNoHelp = 0, kCopiesLooking = 1, kCopiesApplying = 2 };

  void WarnOtherThreadCopies(const char* where) {
    if (!copies_other_warned_thread_) {
      copies_other_warned_thread_ = true;
      REXLOG_ERROR("[native] C6: {} from a thread other than the one that enqueues vertex copies", where);
    }
  }

  bool EnqueueCopy(const WorkCopy& work) {
    if (copies_no_thread_) {
      // Thread switched off by a MISMATCH (or it could not be created): the copy goes on the ring.
      ++copies_inf_.in_line;
      copies_inf_.bytes_in_line += uint64_t(work.words) * 4;
      return false;
    }
    if (!copies_thread_.joinable()) {
      copies_producer_ = std::this_thread::get_id();
      copies_phase_ = copies_requested_help_ ? kCopiesLooking : kCopiesNoHelp;
      const int ring_core = RexSwitchCurrentCore();  // asked by the ring, which creates the thread
      try {
        copies_thread_ = std::thread([this, ring_core] {
          rex::thread::set_current_thread_name("MASSEFFECT vertex copies");
          // masseffect_native_uploads_thread_priority (see the cvar).
          const int32_t priority = REXCVAR_GET(masseffect_native_uploads_thread_priority);
          const bool priority_ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
          // masseffect_native_uploads_thread_core (see the cvar).
          const int32_t request = REXCVAR_GET(masseffect_native_uploads_thread_core);
          const int core = request == -2 ? (ring_core == 2 ? 1 : 2) : int(request);
          const bool core_ok = core < 0 || RexSwitchSetCurrentThreadCore(core);
          REXLOG_INFO("[native] C6: vertex-copy thread at priority {:#x}{}; preferred core {}{} (the ring runs "
                      "on core {}), now on core {}",
                      priority, priority_ok ? "" : " (NOT accepted: keeps the one from its creation)", core,
                      core_ok ? "" : " (NOT accepted)", ring_core, RexSwitchCurrentCore());
          LoopCopies();
        });
      } catch (const std::system_error& error) {
        copies_no_thread_ = true;
        REXLOG_ERROR("[native] C6: could not create the vertex-copy thread ({}): copies go on the ring",
                     error.what());
        return false;
      }
      REXLOG_INFO("[native] C6: vertex copies on a separate thread (masseffect_native_uploads_thread); the ring copies them "
                  "itself when waiting for them (masseffect_native_uploads_help) = {}",
                  copies_requested_help_
                      ? fmt::format("YES, in chunks of {}; LOOKING phase during the first {} waits with pending "
                                    "copies",
                                    kCopiesPerChunk, kWaitsCopiesLooking)
                      : std::string("no, waits for the thread as before"));
    } else if (std::this_thread::get_id() != copies_producer_) {
      WarnOtherThreadCopies("EnqueueCopy");
    }
    const size_t written = written_copies_.load(std::memory_order_relaxed);
    if (written - done_copies_.load(std::memory_order_acquire) >= copies_.size()) {
      ++copies_in_line_;
      ++copies_inf_.in_line;
      copies_inf_.bytes_in_line += uint64_t(work.words) * 4;
      return false;
    }
    copies_[written & (copies_.size() - 1)] = work;
    written_copies_.store(written + 1);
    ++copies_inf_.queued;
    copies_inf_.bytes_queued += uint64_t(work.words) * 4;
    // Wake-up every kCopiesPerWarning copies: waking the thread as soon as it slept woke it on almost every
    // copy, and every wake-up goes through the kernel (1.4 % of the ring and almost all of the copy thread's
    // CPU). Whatever is left without a wake-up is collected by WaitUploads.
    if (((written + 1) & (kCopiesPerWarning - 1)) == 0) {
      WakeCopies();
    }
    return true;
  }

  void WakeCopies() {
    bool warn;
    {
      std::lock_guard<std::mutex> latch(copies_mutex_);
      warn = copies_sleeping_;
    }
    if (warn) {
      copies_cv_.notify_one();
    }
  }

  // Fence measurement only.
  size_t PendingCopies() const override {
    if (!copies_thread_.joinable()) {
      return 0;
    }
    const size_t written = written_copies_.load(std::memory_order_relaxed);
    const size_t done = done_copies_.load(std::memory_order_relaxed);
    return written > done ? written - done : 0;
  }

  void WaitUploads() override {
    if (!copies_thread_.joinable()) {
      return;
    }
    const bool producer = std::this_thread::get_id() == copies_producer_;
    if (!producer) {
      WarnOtherThreadCopies("WaitUploads");
    }
    const size_t goal = written_copies_.load(std::memory_order_relaxed);
    if (done_copies_.load(std::memory_order_acquire) == goal) {
      if (producer) {
        CheckCopies(goal);  // what the thread did since the last wait
      }
      return;
    }
    const auto before = std::chrono::steady_clock::now();
    // What the thread had not taken yet on arrival (while observing, what the ring would have copied).
    copies_inf_.no_take += goal - std::min(goal, taken_copies_.load(std::memory_order_acquire));
    uint64_t ns_helping = 0;
    uint64_t helped = 0;
    if (producer && copies_phase_ == kCopiesApplying) {
      // 1. What the thread has not taken yet, the ring copies in batches: if the thread is running, they
      //    share it.
      uint64_t bytes = 0;
      while (TakeAndCopy(true, helped, bytes)) {
      }
      const auto after_help = std::chrono::steady_clock::now();
      ns_helping = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(after_help - before).count());
      helped_copies_ += helped;
      copies_bytes_helped_ += bytes;
      copies_ns_helping_ += ns_helping;
      copies_inf_.helped += helped;
      copies_inf_.bytes_helped += bytes;
      copies_inf_.ns_helping += ns_helping;
      if (helped) {
        ++copies_inf_.waits_with_help;
      }
      // 2. What is missing is the batch the thread is working on: wait on its mutex, which lends the thread
      //    the ring's priority until it releases it.
      if (done_copies_.load(std::memory_order_acquire) != goal) {
        {
          std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
          if (done_copies_.load(std::memory_order_acquire) != goal) {
            RepairCopies(goal, "with the chunk lock the thread has nothing in hand and copies are still missing");
          }
        }
        const uint64_t ns_chunk = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - after_help)
                                               .count());
        ++copies_inf_.waits_chunk;
        copies_inf_.ns_chunk += ns_chunk;
        copies_inf_.ns_chunk_worst = std::max(copies_inf_.ns_chunk_worst, ns_chunk);
      }
    } else {
      WaitCopiesAsAlways(goal);
    }
    if (producer) {
      CheckCopies(goal);
      if (copies_phase_ == kCopiesLooking && ++copies_waits_looking_ >= kWaitsCopiesLooking) {
        copies_phase_ = kCopiesApplying;
        REXLOG_INFO("[native] C6: vertex copies: {} waits with pending copies in the LOOKING phase and {} copies "
                    "checked, all with their mark and the counters matching. Moves to the APPLYING phase: the ring "
                    "copies what the thread has not taken and waits for the chunk in progress with its lock",
                    copies_waits_looking_, checked_copies_);
      }
    }
    ++waits_copies_;
    const uint64_t ns_total = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - before)
                                           .count());
    // The waiting time now counts only the wait; what the ring copies is counted separately (as helping).
    const uint64_t ns_wait = ns_total > ns_helping ? ns_total - ns_helping : 0;
    ns_waiting_copies_ += ns_wait;
    ++copies_inf_.waits;
    copies_inf_.ns_wait += ns_wait;
    copies_inf_.ns_wait_worst = std::max(copies_inf_.ns_wait_worst, ns_wait);
    // And to the "[hitch] ring" line (masseffect_waits_hitch.h). This is only reached when the ring really
    // waits, a few times per frame: the fetch_add calls do not show.
    masseffect::waits::g_ns_waiting_copies.fetch_add(ns_wait, std::memory_order_relaxed);
    masseffect::waits::g_waits_copies.fetch_add(1, std::memory_order_relaxed);
    masseffect::waits::g_ns_helping_copies.fetch_add(ns_helping, std::memory_order_relaxed);
    masseffect::waits::g_helped_copies.fetch_add(helped, std::memory_order_relaxed);
  }

  // The plain wait (observing and no-help phases, or when another thread waits): the ring sleeps until the
  // thread finishes. After 2 s it no longer keeps waiting forever (the earlier hang): it takes the batch
  // mutex, which lends its priority to the thread if it is copying, and if copies are still missing with
  // the thread out of its loop, the ring does them and switches the thread off.
  void WaitCopiesAsAlways(size_t goal) {
    bool a_time = false;
    {
      std::unique_lock<std::mutex> latch(copies_mutex_);
      if (copies_sleeping_) {
        copies_cv_.notify_one();  // a wake-up EnqueueCopy skipped (it only wakes every kCopiesPerWarning copies)
      }
      copies_waiting_ = true;
      a_time = done_copies_cv_.wait_for(latch, std::chrono::seconds(2), [this, goal] {
        return done_copies_.load(std::memory_order_acquire) == goal;
      });
      copies_waiting_ = false;
    }
    ++copies_inf_.whole_waits;
    if (a_time) {
      return;
    }
    // Should never happen: diagnostic for a hang once seen in the menus.
    REXLOG_ERROR("[native] C6: the ring thread has been waiting 2 s for vertex copies: goal {}, done {}, "
                 "taken {}, written {}, copied by the thread {}",
                 goal, done_copies_.load(std::memory_order_acquire),
                 taken_copies_.load(std::memory_order_acquire), written_copies_.load(std::memory_order_acquire),
                 copies_progress_.load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
    if (done_copies_.load(std::memory_order_acquire) != goal) {
      RepairCopies(goal, "2 s waiting for the thread and, with the thread outside its loop, copies are still missing");
    }
  }

  // Takes the next untaken batch (up to kCopiesPerChunk copies, in order) and copies it. Used by the
  // thread, holding copies_chunk_mutex_, and by the ring when helping. false = nothing was left to take.
  bool TakeAndCopy(bool ring, uint64_t& copies, uint64_t& bytes) {
    const size_t written = written_copies_.load(std::memory_order_acquire);
    size_t taken = taken_copies_.load(std::memory_order_acquire);
    size_t n = 0;
    do {
      if (taken >= written) {
        return false;
      }
      n = std::min<size_t>(written - taken, kCopiesPerChunk);
    } while (!taken_copies_.compare_exchange_weak(taken, taken + n, std::memory_order_acq_rel,
                                                    std::memory_order_acquire));
    const size_t mask = copies_.size() - 1;
    for (size_t i = taken; i < taken + n; ++i) {
      const WorkCopy& work = copies_[i & mask];
      CopyVertices(work);
      bytes += uint64_t(work.words) * 4;
      copies_marks_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
      if (!ring) {
        copies_progress_.store(i + 1, std::memory_order_relaxed);  // for the WaitUploads diagnostic
      }
    }
    copies += n;
    done_copies_.fetch_add(n, std::memory_order_acq_rel);  // publishes the copies and their marks
    return true;
  }

  bool HasCopiesNoTake() const {
    return taken_copies_.load(std::memory_order_acquire) != written_copies_.load(std::memory_order_acquire);
  }

  // The guard, on each wait and with everything finished (the ring does not queue while waiting and the
  // thread has nothing half taken): what was queued since the previous one has its mark and the counters
  // add up. Ring only.
  void CheckCopies(size_t goal) {
    if (goal == verified_copies_ || copies_no_thread_) {
      return;
    }
    const size_t mask = copies_.size() - 1;
    const size_t from =
        std::max(verified_copies_, goal > copies_.size() ? goal - copies_.size() : size_t(0));
    size_t no_mark = 0;
    size_t first = 0;
    for (size_t i = from; i < goal; ++i) {
      if (copies_marks_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        if (!no_mark) {
          first = i;
        }
        ++no_mark;
      }
    }
    checked_copies_ += goal - from;
    copies_inf_.checked += goal - from;
    const size_t taken = taken_copies_.load(std::memory_order_acquire);
    const size_t done = done_copies_.load(std::memory_order_acquire);
    if (!no_mark && taken == goal && done == goal) {
      verified_copies_ = goal;
      return;
    }
    const std::string reason = no_mark ? fmt::format("{} of {} copies without their mark (the first one, copy {})", no_mark,
                                                       goal - from, first)
                                         : std::string("all with their mark, but the counters do not match");
    std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);  // the thread, out of its copy loop
    if (copies_phase_ == kCopiesApplying && taken == goal && done == goal) {
      // With the counters right, the suspect is the help path: redo whatever is missing and go back to the
      // plain wait.
      const size_t redone = RedoNoMark(goal);
      verified_copies_ = goal;
      ++copies_differences_;
      copies_phase_ = kCopiesNoHelp;
      REXLOG_ERROR("[native] C6: vertex copies: MISMATCH: {} ({} enqueued, counters matching; {} redone "
                   "on the ring before submitting). Ring help SWITCHED OFF for the rest of the session: waits for the thread "
                   "as before",
                   reason, goal, redone);
      return;
    }
    RepairCopies(goal, reason);
  }

  // Copies on the ring whatever was queued up to `goal` (since the last check, and at most one lap of
  // the queue) that lacks its mark, and marks it. With copies_chunk_mutex_ held. Returns how many.
  size_t RedoNoMark(size_t goal) {
    const size_t mask = copies_.size() - 1;
    const size_t from =
        std::max(verified_copies_, goal > copies_.size() ? goal - copies_.size() : size_t(0));
    size_t redone = 0;
    for (size_t i = from; i < goal; ++i) {
      if (copies_marks_[i & mask].load(std::memory_order_relaxed) != uint32_t(i + 1)) {
        CopyVertices(copies_[i & mask]);
        copies_marks_[i & mask].store(uint32_t(i + 1), std::memory_order_relaxed);
        ++redone;
      }
    }
    return redone;
  }

  // Should never happen. With copies_chunk_mutex_ held, i.e. with the thread out of its copy loop and
  // nothing half taken: the ring copies whatever is missing up to `goal` (before submitting and before
  // returning the read pointer), the counters are left consistent and the thread is switched off for the
  // session.
  void RepairCopies(size_t goal, const std::string& reason) {
    const size_t taken = taken_copies_.load(std::memory_order_acquire);
    const size_t done = done_copies_.load(std::memory_order_acquire);
    const size_t redone = RedoNoMark(goal);
    taken_copies_.store(goal, std::memory_order_release);
    done_copies_.store(goal, std::memory_order_release);
    verified_copies_ = goal;
    ++copies_differences_;
    copies_no_thread_ = true;
    REXLOG_ERROR("[native] C6: vertex copies: MISMATCH: {} (enqueued {}, taken {}, done {}; {} redone on "
                 "the ring before submitting). Copy thread SWITCHED OFF for the rest of the session: copies go on the "
                 "ring, as before",
                 reason, goal, taken, done, redone);
  }

  const char* PhaseNameCopies() const {
    if (copies_no_thread_) {
      return "NO THREAD (copies on the ring)";
    }
    return copies_phase_ == kCopiesApplying ? "APPLYING" : copies_phase_ == kCopiesLooking ? "LOOKING" : "NO HELP";
  }

  // Every 10 s, if there were copies: who did them and how long the ring really waited. Ring only.
  void ReportCopies() {
    if (!copies_thread_.joinable() && !copies_no_thread_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - copies_report_ < std::chrono::seconds(10)) {
      return;
    }
    copies_report_ = now;
    const uint64_t thread_n = copies_thread_n_.load(std::memory_order_relaxed);
    const uint64_t thread_bytes = copies_thread_bytes_.load(std::memory_order_relaxed);
    const uint64_t d_thread_n = thread_n - copies_thread_n_previous_;
    const uint64_t d_thread_bytes = thread_bytes - copies_thread_bytes_previous_;
    copies_thread_n_previous_ = thread_n;
    copies_thread_bytes_previous_ = thread_bytes;
    const ReportCopiesFigures& c = copies_inf_;
    if (c.queued || c.in_line || c.waits) {
      MASSEFFECT_REPORT_RING(
          "[native] C6 vertex copies, last 10 s: {} enqueued ({:.1f} MB) and {} on the ring without "
          "going through the queue ({:.1f} MB); the thread copied {} ({:.1f} MB) and the ring helped with {} ({:.1f} MB, {:.1f} ms) "
          "in {} of {} waits with pending copies ({} not taken on arrival); waited for the thread's chunk {} times "
          "({:.2f} ms, worst {:.2f} ms) and for all of it {} times; pure wait {:.1f} ms (worst {:.2f} ms); {} copies "
          "checked, {} mismatches; phase {}",
          c.queued, double(c.bytes_queued) / 1048576.0, c.in_line, double(c.bytes_in_line) / 1048576.0,
          d_thread_n, double(d_thread_bytes) / 1048576.0, c.helped, double(c.bytes_helped) / 1048576.0,
          double(c.ns_helping) / 1e6, c.waits_with_help, c.waits, c.no_take, c.waits_chunk,
          double(c.ns_chunk) / 1e6, double(c.ns_chunk_worst) / 1e6, c.whole_waits, double(c.ns_wait) / 1e6,
          double(c.ns_wait_worst) / 1e6, c.checked, copies_differences_, PhaseNameCopies());
    }
    copies_inf_ = ReportCopiesFigures{};
  }

  // The per-fetch-constant sampler table, every 10 s: hits, and misses by cause.
  void ReportCacheFetch() {
    const auto now = std::chrono::steady_clock::now();
    if (now - fetch_report_ < std::chrono::seconds(10)) {
      return;
    }
    fetch_report_ = now;
    const std::array<uint64_t, 6> now_figures{samplers_cache_fetch_, fetch_failures_clash_, fetch_empty_failures_,
                                               fetch_failures_generation_, fetch_expired_failures_, samplers_cache_};
    std::array<uint64_t, 6> d{};
    for (size_t i = 0; i < d.size(); ++i) {
      d[i] = now_figures[i] - fetch_previous_report_[i];
    }
    fetch_previous_report_ = now_figures;
    const uint64_t failures = d[1] + d[2] + d[3] + d[4];
    if (d[0] + failures == 0) {
      return;
    }
    MASSEFFECT_REPORT_RING(
        "[native] C6 per-fetch sampler cache ({} slots), last 10 s: {} register hits, {} "
        "table lookups with {} hits and {} misses ({:.1f} %): {} collisions with another fetch in its slot, {} in "
        "an empty slot, {} by generation and {} expired (the texture must be rechecked)",
        cache_fetch_.size(), d[5], d[0] + failures, d[0], failures,
        100.0 * double(failures) / double(std::max<uint64_t>(d[0] + failures, 1)), d[1], d[2], d[3], d[4]);
  }

  void StopCopies() {
    if (!copies_thread_.joinable()) {
      return;
    }
    {
      std::lock_guard<std::mutex> latch(copies_mutex_);
      copies_stop_ = true;
    }
    copies_cv_.notify_one();
    copies_thread_.join();
    REXLOG_INFO("[native] C6: vertex-copy thread stopped ({} copies on the ring because of a full queue, {} "
                "ring waits, {:.1f} ms waiting; the ring copied {} while waiting ({:.1f} MB, "
                "{:.1f} ms), {} checked, {} mismatches, phase {})",
                copies_in_line_, waits_copies_, double(ns_waiting_copies_) / 1e6, helped_copies_,
                double(copies_bytes_helped_) / 1048576.0, double(copies_ns_helping_) / 1e6, checked_copies_,
                copies_differences_, PhaseNameCopies());
  }

  void LoopCopies() {
    for (;;) {
      if (HasCopiesNoTake()) {
        // Holding copies_chunk_mutex_ from the first batch until there is nothing left to take, and never
        // waiting on anything inside. If the ring waits for the current batch, it waits on this mutex and the
        // thread runs at the ring's priority until it releases it.
        uint64_t copies = 0;
        uint64_t bytes = 0;
        {
          std::lock_guard<std::mutex> chunk(copies_chunk_mutex_);
          while (TakeAndCopy(false, copies, bytes)) {
          }
        }
        copies_thread_n_.fetch_add(copies, std::memory_order_relaxed);
        copies_thread_bytes_.fetch_add(bytes, std::memory_order_relaxed);
        continue;  // look again: if nothing is left, sleep through the path below
      }
      // Nothing to take (the ring may have taken the last ones). Under the lock: wake the ring if it waits,
      // and sleep until there is more. If the read of written_copies_ was stale, the ring signals at its next
      // point that takes the lock.
      std::unique_lock<std::mutex> latch(copies_mutex_);
      if (copies_waiting_) {
        done_copies_cv_.notify_one();
      }
      copies_sleeping_ = true;
      copies_cv_.wait(latch, [this] { return copies_stop_ || HasCopiesNoTake(); });
      copies_sleeping_ = false;
      /*
       * The thread used to die on its own. It exited with "if (!HasCopiesNoTake()) return;", on the
       * assumption that it was only woken without work in order to stop. But the ring takes untaken copies
       * when it waits for them (TakeAndCopy, without copies_mutex_): if the thread woke because there were
       * copies and the ring took them before this check, the thread ended for good. It was seen in
       * a busy scene with the ring at 94 %: from then on "the thread copied 0" in every report, the
       * menu included, and the ring did all the copies (3-4 ms more per frame for the rest of the session).
       * Now it only exits when asked to stop; otherwise it looks again.
       */
      if (copies_stop_ && !HasCopiesNoTake()) {
        return;  // stop, nothing pending
      }
    }
  }

  static bool ResolveFetchAuditEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("MASSEFFECT_NATIVE_AUDIT_RESOLVE_FETCH");
      return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
  }

  void AuditFetchResolved(uint32_t base, const uint32_t* fetch, const ImageNative& image) {
    if (!ResolveFetchAuditEnabled()) return;
    if (me_resolve_fetch_contracts_.size() >= 256) {
      if (!me_resolve_fetch_cap_logged_) {
        me_resolve_fetch_cap_logged_ = true;
        REXLOG_INFO("[native] resolved fetch audit reached 256-contract cap at frame {}; "
                    "later absence of contracts is not evidence of matching layouts", frame_);
      }
      return;
    }
    const uint32_t guest_format = fetch[1] & 63u;
    const uint32_t width = (fetch[2] & 8191u) + 1;
    const uint32_t height = ((fetch[2] >> 13) & 8191u) + 1;
    const auto base_format = [](uint32_t value) {
      return uint32_t(rex::graphics::GetBaseFormat(rex::graphics::xenos::TextureFormat(value)));
    };
    const uint32_t contract[] = {base, guest_format, image.resolved_guest_format,
        uint32_t(image.format), width, height, image.width, image.height,
        me_resolve_fetch_vs_, uint32_t(me_resolve_fetch_ps_), me_resolve_fetch_sampler_,
        fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]};
    if (!me_resolve_fetch_contracts_.insert(XXH3_64bits(contract, sizeof(contract))).second) return;
    REXLOG_INFO("[native] resolved fetch contract frame {} VS n{} PS n{} sampler {}: "
                "base {:08X}, fetch guest {} {}x{} pitch {}, latest resolve guest {} "
                "host {} {}x{}, exact guest-format match {}, SDK base-format match {}, "
                "logical extent match {}, swizzle {:03X}, endian {}; "
                "diagnostic only, no reinterpretation or correction",
                frame_, me_resolve_fetch_vs_, me_resolve_fetch_ps_, me_resolve_fetch_sampler_,
                base, guest_format, width, height, ((fetch[0] >> 22) & 511u) << 5,
                image.resolved_guest_format, uint32_t(image.format), image.width, image.height,
                guest_format == image.resolved_guest_format,
                image.resolved_guest_format < 64 && base_format(guest_format) == base_format(image.resolved_guest_format),
                width == image.width && height == image.height,
                (fetch[3] >> 1) & 4095u, (fetch[1] >> 6) & 3u);
  }

  static bool PayloadAuditEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("MASSEFFECT_NATIVE_AUDIT_PAYLOAD_STABILITY");
      return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
  }

  bool StartPayloadAudit(uint32_t kind, uint64_t bytes) {
    if (!PayloadAuditEnabled() || !me_payload_hdr_ || !bytes) return false;
    constexpr uint64_t kByteCap = 128ull << 20;
    if (me_payload_checked_[kind] >= 256 || bytes > kByteCap - me_payload_bytes_[kind]) {
      if (++me_payload_skipped_[kind] == 1) ReportPayloadAudit("limit/oversized");
      return false;
    }
    ++me_payload_checked_[kind];
    me_payload_bytes_[kind] += bytes;
    return true;
  }

  void ReportPayloadAudit(const char* reason) {
    if (!PayloadAuditEnabled()) return;
    REXLOG_INFO("[native] payload stability {}: texture levels {} / {} bytes, source changes {}, "
                "staged disagreements {}, cap skips {}; vertex copies {} / {} bytes, source changes {}, "
                "staged disagreements {}, cap skips {}; independent caps 256/128MiB, afterHDR {}, "
                "full USED bytes only, invalid audit ranges {}; no coverage of async vertices, texture cache hits, "
                "cache fingerprint-to-level gaps, worker snapshots, 3D textures or LOAD_ALU; "
                "zero discrepancies is not proof of no data races",
                reason, me_payload_checked_[0], me_payload_bytes_[0], me_payload_changed_[0],
                me_payload_staged_[0], me_payload_skipped_[0], me_payload_checked_[1], me_payload_bytes_[1],
                me_payload_changed_[1], me_payload_staged_[1], me_payload_skipped_[1], me_payload_hdr_,
                me_payload_invalid_);
  }

  void FinalizePayloadAudit(uint32_t kind, uint64_t address, uint64_t bytes,
                             uint64_t before, uint64_t after, uint64_t staged) {
    if (before != after) ++me_payload_changed_[kind];
    if (staged != before || staged != after) ++me_payload_staged_[kind];
    if ((before != after || staged != before || staged != after) && ++me_payload_logged_[kind] <= 8) {
      REXLOG_WARN("[native] payload stability {} discrepancy: address {:08X}, {} full used bytes, "
                  "frame {} VS {} PS {}, before {:016X} staged {:016X} after {:016X}; measurement only",
                  kind ? "vertex" : "texture-level", address, bytes, frame_, me_payload_vs_,
                  me_payload_ps_, before, staged, after);
    }
    if (me_payload_checked_[kind] == 1 || me_payload_checked_[kind] % 32 == 0)
      ReportPayloadAudit("coverage");
  }

  void AuditCacheConstants(bool vs, uint32_t shader, uint64_t generation,
                              const uint32_t* current, VkDeviceSize offset, uint32_t bytes) {
    static const bool enabled = [] {
      const char* value = std::getenv("MASSEFFECT_NATIVE_AUDIT_CONSTANT_CACHE");
      return value && std::strcmp(value, "1") == 0;
    }();
    constexpr uint64_t kLimit = 20000;
    if (!enabled || !me_constant_audit_hdr_ || !bytes ||
        me_constant_audit_checked_[0] + me_constant_audit_checked_[1] >= kLimit) return;
    const uint32_t bank = vs ? 0 : 1;
    if (offset > used_upload_ || bytes > used_upload_ - offset) {
      REXLOG_ERROR("[native] constant cache audit invalid {} offset {} bytes {} used {}",
                   vs ? "VS" : "PS", offset, bytes, used_upload_);
      return;
    }
    ++me_constant_audit_checked_[bank];
    me_constant_audit_bytes_[bank] += bytes;
    const uint8_t* uploaded = upload_data_ + offset;
    if (std::memcmp(uploaded, current, bytes) != 0) {
      if (++me_constant_audit_mismatches_[bank] <= 8) {
        uint32_t word = 0, previous = 0;
        for (; word < bytes / 4; ++word) {
          std::memcpy(&previous, uploaded + word * 4, 4);
          if (previous != current[word]) break;
        }
        REXLOG_WARN("[native] constant cache MISMATCH {} n{} generation {} epoch {} "
                    "c{}.{} uploaded {:08X} current {:08X} bytes {}",
                    vs ? "VS" : "PS", shader, generation, epoch_upload_, word / 4,
                    word % 4, previous, current[word], bytes);
      }
    }
    const uint64_t checked = me_constant_audit_checked_[0] + me_constant_audit_checked_[1];
    if (checked == 1 || (checked & 2047) == 0 || checked == kLimit) {
      REXLOG_INFO("[native] constant cache audit after HDR: VS {} comparisons / {} mismatches / {} bytes; "
                  "PS {} comparisons / {} mismatches / {} bytes; limit {}{}",
                  me_constant_audit_checked_[0], me_constant_audit_mismatches_[0], me_constant_audit_bytes_[0],
                  me_constant_audit_checked_[1], me_constant_audit_mismatches_[1], me_constant_audit_bytes_[1],
                  kLimit, checked == kLimit ? " reached" : "");
    }
  }

  bool Reserve(VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize& offset) {
    const VkDeviceSize start = (used_upload_ + alignment - 1) & ~(alignment - 1);
    if (start + bytes > kUploadSize) {
      offset = 0;
      return false;  // cannot happen: Draw checks the space first
    }
    offset = start;
    used_upload_ = start + bytes;
    megabytes_bytes_ += bytes;
    return true;
  }

  // Like Reserve, but the offset is a multiple of `multiple` (the stride: a multiple of 4, not always a
  // power of 2). Draw has already asked for space including that extra padding (gap_base_zero).
  bool ReserveMultiple(VkDeviceSize bytes, VkDeviceSize multiple, VkDeviceSize& offset) {
    const VkDeviceSize start = (used_upload_ + multiple - 1) / multiple * multiple;
    if (start + bytes > kUploadSize) {
      offset = 0;
      return false;  // cannot happen: Draw checks the space first
    }
    offset = start;
    used_upload_ = start + bytes;
    megabytes_bytes_ += bytes;
    return true;
  }

  // The pipelines file (cache/masseffect_native_pipelines.bin): the Vulkan cache and the prewarm list.
  static std::filesystem::path PathFilePipelines() {
    // Isolate diagnostic runs from the shared driver cache without deleting it.
    // MoltenVK may compile every stored library while loading initial data.
    static const char* const diagnostic_path = std::getenv("MASSEFFECT_NATIVE_PIPELINE_CACHE_PATH");
    if (diagnostic_path && *diagnostic_path) return std::filesystem::path(diagnostic_path);
    // Cold-start test (masseffect_cold_startup): a separate file that the test cycle deletes first, so the
    // run starts with no pipeline cache and no prewarm list, without touching the normal cache.
    if (REXCVAR_GET(masseffect_cold_startup))
      return rex::filesystem::GetExecutableFolder() / kFolderCache / "cold.bin";
    return rex::filesystem::GetExecutableFolder() / kFolderCache / kFilePipelines;
  }

  static void ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& data) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const std::streamoff bytes = file ? std::streamoff(file.tellg()) : 0;
    if (bytes > 0 && bytes < (std::streamoff(256) << 20)) {
      data.resize(size_t(bytes));
      file.seekg(0);
      if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(bytes))) {
        data.clear();
      }
    }
  }

  // Reads the pipelines file and splits it into the cache and the list. If it does not exist yet, the two
  // older files (next to the NRO), which are deleted when the new one is written.
  void ReadFilePipelines(std::vector<uint8_t>& cache, std::vector<uint8_t>& list) {
    std::vector<uint8_t> all;
    ReadWholeFile(PathFilePipelines(), all);
    if (!all.empty()) {
      uint32_t magic = 0;
      uint32_t version = 0;
      uint64_t bytes_list = 0;
      uint64_t bytes_cache = 0;
      if (all.size() >= kHeaderFilePipelines) {
        std::memcpy(&magic, all.data(), 4);
        std::memcpy(&version, all.data() + 4, 4);
        std::memcpy(&bytes_list, all.data() + 8, 8);
        std::memcpy(&bytes_cache, all.data() + 16, 8);
      }
      if (magic != kMagicFilePipelines || version != kVersionFilePipelines ||
          bytes_list > all.size() || bytes_cache > all.size() ||
          kHeaderFilePipelines + bytes_list + bytes_cache != all.size()) {
        REXLOG_WARN("[native] C6: pipeline file from another version or damaged: starting from scratch");
        return;
      }
      const auto start = all.begin() + kHeaderFilePipelines;
      list.assign(start, start + std::ptrdiff_t(bytes_list));
      cache.assign(start + std::ptrdiff_t(bytes_list), all.end());
      return;
    }
    // An explicit new diagnostic cache must not import the legacy shared files.
    const char* const diagnostic_path = std::getenv("MASSEFFECT_NATIVE_PIPELINE_CACHE_PATH");
    if (diagnostic_path && *diagnostic_path) return;
    const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
    ReadWholeFile(folder / kFileCacheOld, cache);
    ReadWholeFile(folder / kFileOldList, list);
    old_files_ = !cache.empty() || !list.empty();
  }

  // On-disk pipeline cache (derived from the game's shaders: not distributed). If the driver rejects it
  // (another driver or corrupt data), it starts from scratch.
  void LoadCachePipelines() {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    const auto create = reinterpret_cast<FnCreateCachePipelines>(
        deferred::Proc(ifn.vkGetDeviceProcAddr, device_, "vkCreatePipelineCache"));
    data_cache_ = reinterpret_cast<FnDataCachePipelines>(
        deferred::Proc(ifn.vkGetDeviceProcAddr, device_, "vkGetPipelineCacheData"));
    destroy_cache_ = reinterpret_cast<FnDestroyCachePipelines>(
        deferred::Proc(ifn.vkGetDeviceProcAddr, device_, "vkDestroyPipelineCache"));
    if (!create || !data_cache_ || !destroy_cache_) {
      REXLOG_WARN("[native] C6: no pipeline cache (the driver does not provide its functions)");
      return;
    }
    const std::filesystem::path path = PathFilePipelines();
    std::vector<uint8_t> data;
    ReadFilePipelines(data, read_list_);
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (create(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
      info.initialDataSize = 0;
      info.pInitialData = nullptr;
      data.clear();
      if (create(device_, &info, nullptr, &cache_pipelines_) != VK_SUCCESS) {
        cache_pipelines_ = VK_NULL_HANDLE;
      }
    }
    bytes_cache_saved_ = data.size();
    cache_saved_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[native] C6: pipeline cache {} ({} KB read from {}{})",
                cache_pipelines_ != VK_NULL_HANDLE ? "active" : "not available",
                data.size() >> 10, path.string(), old_files_ ? ", from the two earlier files" : "");
    // The writer thread always saves both parts; it starts from what is on disk.
    written_cache_ = std::move(data);
    if (old_files_) {
      // they came from the two older files: moved to the new one on the first save even if nothing changed
      bytes_cache_saved_ = 0;
      pipelines_no_save_ = 1;
      list_no_save_ = 1;
    }
    LoadPipelinesList();  // the prewarm list
  }

  void SaveCachePipelines() {
    cache_saved_ = std::chrono::steady_clock::now();
    // The prewarm list is saved even if the cache does not grow (a new pipeline whose shaders were already
    // in it does not make it grow). The cache, as usual: only if it has new entries.
    std::vector<uint8_t> list;
    if (list_no_save_) {
      list_no_save_ = 0;
      list = SerializePipelinesList();
    }
    std::vector<uint8_t> data;
    if (pipelines_no_save_) {
      pipelines_no_save_ = 0;
      size_t bytes = 0;
      // without new entries it is not rewritten (the SD card is slow on the Switch)
      if (cache_pipelines_ != VK_NULL_HANDLE &&
          data_cache_(device_, cache_pipelines_, &bytes, nullptr) == VK_SUCCESS && bytes &&
          bytes != bytes_cache_saved_) {
        data.resize(bytes);
        const VkResult result = data_cache_(device_, cache_pipelines_, &bytes, data.data());
        if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !bytes) {
          data.clear();
        } else {
          data.resize(bytes);
          bytes_cache_saved_ = bytes;
        }
      }
    }
    if (data.empty() && list.empty()) {
      return;
    }
    // The file is written on its own thread: on the Switch, writing about 2 MB to the SD card from the ring
    // thread stalled it on every save. If the previous one has not been written yet, this one replaces it.
    std::lock_guard<std::mutex> latch(writer_mutex_);
    if (!writer_cache_.joinable()) {
      writer_cache_ = std::thread([this] {
        rex::thread::set_current_thread_name("MASSEFFECT cache de pipelines");
        WriterCacheMain();
      });
    }
    if (!data.empty()) {  // only the list, if the cache did not grow
      writer_data_ = std::move(data);
    }
    if (!list.empty()) {
      writer_list_ = std::move(list);
    }
    pending_writer_ = true;
    writer_warning_.notify_one();
  }

  // Pipeline cache writer thread. Sleeping or continuing is decided with the lock held.
  void WriterCacheMain() {
    std::vector<uint8_t> data;
    std::vector<uint8_t> list;  // the prewarm one
    for (;;) {
      {
        std::unique_lock<std::mutex> latch(writer_mutex_);
        writer_warning_.wait(latch, [this] { return pending_writer_ || writer_stop_; });
        if (!pending_writer_) {
          return;  // stop, nothing pending
        }
        data = std::move(writer_data_);
        writer_data_ = {};
        list = std::move(writer_list_);
        writer_list_ = {};
        pending_writer_ = false;
      }
      // A single file with both parts. Only one may arrive (the cache did not grow, or the list did not
      // change): the other is the last one written.
      if (!data.empty()) {
        written_cache_ = std::move(data);
      }
      if (!list.empty()) {
        written_list_ = std::move(list);
      }
      WriteFilePipelines();
    }
  }

  // cache/masseffect_native_pipelines.bin with the list and the cache, written to a temporary file that is then
  // renamed. Writer thread only. The first time it is written, the two older files are deleted if present.
  void WriteFilePipelines() {
    const auto before = std::chrono::steady_clock::now();
    const std::filesystem::path path = PathFilePipelines();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::filesystem::path temporal = path;
    temporal += ".tmp";
    {
      const uint32_t magic = kMagicFilePipelines;
      const uint32_t version = kVersionFilePipelines;
      const uint64_t bytes_list = written_list_.size();
      const uint64_t bytes_cache = written_cache_.size();
      std::ofstream file(temporal, std::ios::binary | std::ios::trunc);
      if (!file || !file.write(reinterpret_cast<const char*>(&magic), 4) ||
          !file.write(reinterpret_cast<const char*>(&version), 4) ||
          !file.write(reinterpret_cast<const char*>(&bytes_list), 8) ||
          !file.write(reinterpret_cast<const char*>(&bytes_cache), 8) ||
          !file.write(reinterpret_cast<const char*>(written_list_.data()), std::streamsize(bytes_list)) ||
          !file.write(reinterpret_cast<const char*>(written_cache_.data()), std::streamsize(bytes_cache))) {
        return;
      }
    }
    error.clear();
    std::filesystem::rename(temporal, path, error);
    if (error) {
      std::filesystem::remove(path, error);
      std::filesystem::rename(temporal, path, error);
    }
    if (!error && old_files_) {
      const std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
      std::error_code no_importance;
      std::filesystem::remove(folder / kFileCacheOld, no_importance);
      std::filesystem::remove(folder / kFileOldList, no_importance);
      old_files_ = false;
    }
    if (saved_cache_++ < 8) {
      REXLOG_INFO("[native] C6: pipelines saved ({} KB of cache and {} in the prewarm list, in {} ms, from "
                  "its thread{})",
                  written_cache_.size() >> 10,
                  written_list_.size() >= kHeaderList
                      ? (written_list_.size() - kHeaderList) / sizeof(RegisterPipeline)
                      : 0,
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before)
                      .count(),
                  error ? ", could not rename" : "");
    }
  }

  void StopWriterCache() {
    {
      std::lock_guard<std::mutex> latch(writer_mutex_);
      writer_stop_ = true;
      writer_warning_.notify_one();
    }
    if (writer_cache_.joinable()) {
      writer_cache_.join();
    }
  }

  // One upload buffer per work slot of the render target code: the previous frame's may still be in use
  // on the GPU while the other is filled.
  bool CreateUpload() {
    for (BufferUpload& s : uploads_) {
      const bool created = CreateBufferUpload();
      s = {upload_, upload_memory_, upload_real_size_, upload_data_, upload_address_};
      upload_ = VK_NULL_HANDLE;
      upload_memory_ = VK_NULL_HANDLE;
      upload_data_ = nullptr;
      if (!created) {
        return false;
      }
    }
    const auto& types = vulkan_device_->memory_types();
    REXLOG_INFO("[native] C6: upload buffer in memory type {} ({}, {}; masseffect_native_upload_memory = {})",
                upload_type_, ((types.host_cached >> upload_type_) & 1) ? "with CPU cache" : "without CPU cache",
                coherent_upload_ ? "coherent, no publishing needed" : "published before submitting",
                REXCVAR_GET(masseffect_native_upload_memory));
    MeasureMemoryUpload();
    CreateSharedSeparate();
    UseSlot(0);
    return true;
  }

  // A small CPU-cached buffer per slot, only for the shared constants. If anything fails, it continues as
  // before (in the upload buffer) and says so in the log.
  void CreateSharedSeparate() {
    shared_separate_ = false;
    if (!REXCVAR_GET(masseffect_shared_native_cache)) {
      REXLOG_INFO("[native] C6: shared constants in the upload buffer (masseffect_shared_native_cache = false)");
      return;
    }
    const auto& types = vulkan_device_->memory_types();
    for (BufferUpload& c : shared_bufs_) {
      VkBufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      info.size = kSharedSize;
      info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (dfn_.vkCreateBuffer(device_, &info, nullptr, &c.buffer) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not create the shared-constants buffer; they go in the upload buffer");
        return;
      }
      VkMemoryRequirements requirements;
      dfn_.vkGetBufferMemoryRequirements(device_, c.buffer, &requirements);
      uint32_t type = 0;
      if (!rex::bit_scan_forward(requirements.memoryTypeBits & types.host_visible & types.host_cached, &type)) {
        REXLOG_WARN("[native] C6: no visible memory with CPU cache; shared constants go in the upload buffer");
        return;
      }
      VkMemoryAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserve.allocationSize = requirements.size;
      reserve.memoryTypeIndex = type;
      if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &c.memory) != VK_SUCCESS ||
          dfn_.vkBindBufferMemory(device_, c.buffer, c.memory, 0) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not allocate the shared-constants buffer; they go in the upload buffer");
        return;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, c.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        REXLOG_WARN("[native] C6: could not map the shared-constants buffer; they go in the upload buffer");
        return;
      }
      c.data = static_cast<uint8_t*>(mapped);
      c.real_size = requirements.size;
      shared_type_ = type;
    }
    shared_coherent_ = (types.host_coherent >> shared_type_) & 0x1;
    shared_separate_ = true;
    REXLOG_INFO("[native] C6: shared constants separate, in memory type {} (with CPU cache, {}), {} MB "
                "per slot",
                shared_type_, shared_coherent_ ? "coherent" : "published before submitting",
                kSharedSize >> 20);
  }

  bool CreateBufferUpload() {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = kUploadSize;
    // UNIFORM_BUFFER because it is also bound as a dynamic UBO (constants through UBOs, set 4).
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &upload_) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements;
    dfn_.vkGetBufferMemoryRequirements(device_, upload_, &requirements);
    // masseffect_native_upload_memory: with 1 or 2, a host-visible type with or without CPU caching is looked
    // for; if there is none, the SDK's.
    const auto& types = vulkan_device_->memory_types();
    upload_type_ = UINT32_MAX;
    const int32_t preference = REXCVAR_GET(masseffect_native_upload_memory);
    if (preference == 1 || preference == 2) {
      const uint32_t visible = requirements.memoryTypeBits & types.host_visible;
      uint32_t type = 0;
      if (rex::bit_scan_forward(visible & (preference == 1 ? types.host_cached : ~types.host_cached), &type)) {
        upload_type_ = type;
      }
    }
    if (upload_type_ == UINT32_MAX) {
      upload_type_ = rex::ui::vulkan::util::ChooseMemoryType(types, requirements.memoryTypeBits,
                                                             rex::ui::vulkan::util::MemoryPurpose::kUpload);
    }
    if (upload_type_ == UINT32_MAX) {
      return false;
    }
    coherent_upload_ = (vulkan_device_->memory_types().host_coherent >> upload_type_) & 0x1;
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    reserve.pNext = &flags;
    reserve.allocationSize = requirements.size;
    reserve.memoryTypeIndex = upload_type_;
    if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &upload_memory_) != VK_SUCCESS) {
      return false;
    }
    upload_real_size_ = requirements.size;
    if (dfn_.vkBindBufferMemory(device_, upload_, upload_memory_, 0) != VK_SUCCESS) {
      return false;
    }
    void* mapped = nullptr;
    if (dfn_.vkMapMemory(device_, upload_memory_, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      return false;
    }
    upload_data_ = static_cast<uint8_t*>(mapped);
    VkBufferDeviceAddressInfo address{};
    address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    address.buffer = upload_;
    upload_address_ = buffer_address_(device_, &address);
    return upload_address_ != 0;
  }

  // For the masseffect_native_upload_memory A/B test: CPU write MB/s into each host-visible memory type and the
  // cost of publishing those 8 MB (vkFlushMappedMemoryRanges; in NVK for Tegra, armDCacheClean). Once.
  void MeasureMemoryUpload() {
    if (!REXCVAR_GET(masseffect_native_diag_memory_upload)) {
      return;
    }
    constexpr VkDeviceSize kBytes = VkDeviceSize(8) << 20;
    std::vector<uint8_t> source(static_cast<size_t>(kBytes));
    for (size_t i = 0; i < source.size(); ++i) {
      source[i] = uint8_t((i * 131) ^ (i >> 11));
    }
    const auto& types = vulkan_device_->memory_types();
    std::string report;
    for (uint32_t type = 0; type < 32; ++type) {
      if (!((types.host_visible >> type) & 1)) {
        continue;
      }
      VkMemoryAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      reserve.allocationSize = kBytes;
      reserve.memoryTypeIndex = type;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      if (dfn_.vkAllocateMemory(device_, &reserve, nullptr, &memory) != VK_SUCCESS) {
        continue;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        using Clock = std::chrono::steady_clock;
        std::memcpy(mapped, source.data(), size_t(kBytes));  // first pass: pages and caches
        const auto before = Clock::now();
        std::memcpy(mapped, source.data(), size_t(kBytes));
        const auto copied = Clock::now();
        const bool coherent = (types.host_coherent >> type) & 1;
        if (!coherent) {
          VkMappedMemoryRange range{};
          range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
          range.memory = memory;
          range.size = VK_WHOLE_SIZE;
          dfn_.vkFlushMappedMemoryRanges(device_, 1, &range);
        }
        const auto published = Clock::now();
        const double seconds = std::chrono::duration<double>(copied - before).count();
        report += fmt::format(
            "{}type {} ({}{}): {:.0f} MB/s{}", report.empty() ? "" : "; ", type,
            ((types.host_cached >> type) & 1) ? "with CPU cache" : "without CPU cache", coherent ? ", coherent" : "",
            seconds > 0 ? 8.0 / seconds : 0.0,
            coherent ? std::string()
                      : fmt::format(", publish {:.2f} ms",
                                    std::chrono::duration<double, std::milli>(published - copied).count()));
        dfn_.vkUnmapMemory(device_, memory);
      }
      dfn_.vkFreeMemory(device_, memory, nullptr);
    }
    REXLOG_INFO("[native] C6: CPU write speed per memory type (8 MB): {}",
                report.empty() ? std::string("no measurable types") : report);
  }

  bool CreateDescriptors() {
    static constexpr VkDescriptorType kTypes[4] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER};
    const VkDescriptorBindingFlags flags_binding =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const auto get_layout_support = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSupport>(
        deferred::Proc(vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr, device_, "vkGetDescriptorSetLayoutSupport"));
    if (!get_layout_support) return false;
    for (uint32_t i = 0; i < 4; ++i) {
      VkDescriptorSetLayoutBinding binding{};
      binding.binding = 0;
      binding.descriptorType = kTypes[i];
      binding.descriptorCount = kCapacityHeap[i];
      binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo flags{};
      flags.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
      flags.bindingCount = 1;
      flags.pBindingFlags = &flags_binding;
      VkDescriptorSetLayoutCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      info.pNext = &flags;
      info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
      info.bindingCount = 1;
      info.pBindings = &binding;
      // The shader heaps are runtime arrays. Size the actual layout to what
      // this device supports instead of creating an invalid 4096-entry set
      // (MoltenVK reports maxPerSetDescriptors=1212 on the current host).
      VkDescriptorSetLayoutSupport support{};
      support.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT;
      for (;;) {
        get_layout_support(device_, &info, &support);
        if (support.supported) break;
        if (binding.descriptorCount <= 1) {
          REXLOG_ERROR("[native] descriptor heap {} is unsupported", i);
          return false;
        }
        binding.descriptorCount /= 2;
      }
      REXLOG_INFO("[native] descriptor heap {}: {} supported entries (requested {})",
                  i, binding.descriptorCount, kCapacityHeap[i]);
      if (dfn_.vkCreateDescriptorSetLayout(device_, &info, nullptr, &layouts_[i]) != VK_SUCCESS) {
        return false;
      }
      heaps_[i].capacity = binding.descriptorCount;
    }
    const VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         heaps_[0].capacity + heaps_[1].capacity + heaps_[2].capacity},
        {VK_DESCRIPTOR_TYPE_SAMPLER, heaps_[3].capacity}};
    VkDescriptorPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    info_pool.maxSets = 4;
    info_pool.poolSizeCount = 2;
    info_pool.pPoolSizes = sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &pool_) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve.descriptorPool = pool_;
    reserve.descriptorSetCount = 4;
    reserve.pSetLayouts = layouts_.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reserve, sets_.data()) != VK_SUCCESS) {
      return false;
    }
    // --- Set 4, the constants through dynamic UBOs ------------------------------------------------------
    // Always created: the shaders of the current library use it statically even with the bit off (it is
    // then bound with offsets 0). With an older library it is unnecessary and harmless. No
    // UPDATE_AFTER_BIND: dynamic descriptors do not support it. CreateUpload runs first, so the buffers
    // already exist.
    use_ubo_ = REXCVAR_GET(masseffect_native_constants_ubo);
    cache_between_frames_ = REXCVAR_GET(masseffect_native_cache_textures_between_frames);
    REXLOG_INFO("[native] C6: texture caches across frames (masseffect_native_cache_textures_between_frames) = {}",
                cache_between_frames_ ? "YES" : "no");
    mipmaps_ = REXCVAR_GET(masseffect_native_mipmaps);
    REXLOG_INFO("[native] C3: texture mip levels (masseffect_native_mipmaps) = {}", mipmaps_ ? "YES" : "no");
    textures_mb_max_ = REXCVAR_GET(masseffect_native_textures_mb_max);
    REXLOG_INFO("[native] C3: texture cache limit (masseffect_native_textures_mb_max) = {} MB{}", textures_mb_max_,
                textures_mb_max_ > 0 ? "" : " (no limit)");
    alignment_ubo_ = std::max<VkDeviceSize>(16, vulkan_device_->properties().minUniformBufferOffsetAlignment);
    REXLOG_INFO("[native] C6: constants through dynamic UBO (masseffect_native_constants_ubo) = {}; alignment {} bytes",
                use_ubo_ ? "YES" : "no", alignment_ubo_);
    std::array<VkDescriptorSetLayoutBinding, 3> bindings_ubo{};
    for (uint32_t b = 0; b < 3; ++b) {
      bindings_ubo[b].binding = b;
      bindings_ubo[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      bindings_ubo[b].descriptorCount = 1;
      bindings_ubo[b].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info_ubo{};
    info_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_ubo.bindingCount = 3;
    info_ubo.pBindings = bindings_ubo.data();
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_ubo, nullptr, &layout_ubo_) != VK_SUCCESS) {
      return false;
    }
    const VkDescriptorPoolSize ubo_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * uint32_t(sets_ubo_.size())};
    VkDescriptorPoolCreateInfo info_pool_ubo{};
    info_pool_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_ubo.maxSets = uint32_t(sets_ubo_.size());
    info_pool_ubo.poolSizeCount = 1;
    info_pool_ubo.pPoolSizes = &ubo_size;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_ubo, nullptr, &pool_ubo_) != VK_SUCCESS) {
      return false;
    }
    std::array<VkDescriptorSetLayout, kSlotsOfWork> layouts_ubo;
    layouts_ubo.fill(layout_ubo_);
    VkDescriptorSetAllocateInfo reserve_ubo{};
    reserve_ubo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve_ubo.descriptorPool = pool_ubo_;
    reserve_ubo.descriptorSetCount = uint32_t(sets_ubo_.size());
    reserve_ubo.pSetLayouts = layouts_ubo.data();
    if (dfn_.vkAllocateDescriptorSets(device_, &reserve_ubo, sets_ubo_.data()) != VK_SUCCESS) {
      return false;
    }
    for (size_t slot = 0; slot < sets_ubo_.size(); ++slot) {
      const VkDescriptorBufferInfo blocks[3] = {{uploads_[slot].buffer, 0, kUboBytesVs},
                                                 {uploads_[slot].buffer, 0, kUboBytesPs},
                                                 {shared_separate_ ? shared_bufs_[slot].buffer
                                                                      : uploads_[slot].buffer,
                                                  0, kUboBytesShared}};
      std::array<VkWriteDescriptorSet, 3> writes_ubo{};
      for (uint32_t b = 0; b < 3; ++b) {
        writes_ubo[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes_ubo[b].dstSet = sets_ubo_[slot];
        writes_ubo[b].dstBinding = b;
        writes_ubo[b].descriptorCount = 1;
        writes_ubo[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes_ubo[b].pBufferInfo = &blocks[b];
      }
      dfn_.vkUpdateDescriptorSets(device_, 3, writes_ubo.data(), 0, nullptr);
    }
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    24};
    VkPipelineLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    const std::array<VkDescriptorSetLayout, 5> layouts_pipeline = {layouts_[0], layouts_[1], layouts_[2],
                                                                    layouts_[3], layout_ubo_};
    info_layout.setLayoutCount = 5;
    info_layout.pSetLayouts = layouts_pipeline.data();
    info_layout.pushConstantRangeCount = 1;
    info_layout.pPushConstantRanges = &range;
    return dfn_.vkCreatePipelineLayout(device_, &info_layout, nullptr, &layout_pipeline_) ==
           VK_SUCCESS;
  }

  // Slot 0 of each heap: a transparent black texture and a basic sampler, as the emulation does for an
  // invalid fetch constant.
  bool CreateEmpty() {
    const std::array<std::pair<VkImageViewType, uint32_t>, 3> types = {
        {{VK_IMAGE_VIEW_TYPE_2D, 1}, {VK_IMAGE_VIEW_TYPE_3D, 1}, {VK_IMAGE_VIEW_TYPE_CUBE, 6}}};
    for (uint32_t i = 0; i < 3; ++i) {
      VkImageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = i == 1 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
      info.flags = i == 2 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
      info.format = VK_FORMAT_R8G8B8A8_UNORM;
      info.extent = {1, 1, 1};
      info.mipLevels = 1;
      info.arrayLayers = types[i].second;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
              empty_[i].image, empty_[i].memory)) {
        return false;
      }
      VkImageViewCreateInfo view{};
      view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      view.image = empty_[i].image;
      view.viewType = types[i].first;
      view.format = VK_FORMAT_R8G8B8A8_UNORM;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, types[i].second};
      if (dfn_.vkCreateImageView(device_, &view, nullptr, &empty_[i].view) != VK_SUCCESS) {
        return false;
      }
      empty_[i].width = empty_[i].height = 1;
      empty_[i].format = VK_FORMAT_R8G8B8A8_UNORM;
      WriteImage(i, 0, empty_[i].view);
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = VK_LOD_CLAMP_NONE;
    if (dfn_.vkCreateSampler(device_, &sampler, nullptr, &sampler_empty_) != VK_SUCCESS) {
      return false;
    }
    WriteSampler(0, sampler_empty_);
    return true;
  }

  // The empty ones are initialized on the first submission: GENERAL and cleared to zero.
  bool PrepareEmpty() {
    if (empty_prepared_) {
      return true;
    }
    const VkCommandBuffer upload = context_->CommandsUpload();
    if (!upload) {
      return false;
    }
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t layers = i == 2 ? 6 : 1;
      Barrier(upload, empty_[i].image, layers);
      VkClearColorValue zero{};
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
      dfn_.vkCmdClearColorImage(upload, empty_[i].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                                &range);
      empty_[i].prepared = true;
    }
    empty_prepared_ = true;
    return true;
  }

  void Barrier(VkCommandBuffer cmd, VkImage image, uint32_t layers) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, layers};  // and mips
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
  }

  void WriteImage(uint32_t heap, uint32_t slot, VkImageView view) {
    VkDescriptorImageInfo image{};
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets_[heap];
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  void WriteSampler(uint32_t slot, VkSampler sampler) {
    VkDescriptorImageInfo image{};
    image.sampler = sampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = sets_[3];
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  uint32_t ReserveSlot(uint32_t heap) {
    auto& m = heaps_[heap];
    if (!m.free.empty()) {
      const uint32_t slot = m.free.back();
      m.free.pop_back();
      return slot;
    }
    return m.next < m.capacity ? m.next++ : 0;
  }

  // The draw's vertex input, recomputed only when the patched VS changes.
  const VerticesEntry* EntryFor(const SubmissionDraw& p) {
    if (generation_entry_ == p.generation_vs && vs_entry_ == p.vs) {
      if (valid_entry_) {
        ++reused_inputs_;
        return entry_current_;
      }
      // The same input that already failed: no recomputation and no warning.
      ++rejected_;
      ++causes_[cause_entry_];
      return nullptr;
    }
    generation_entry_ = p.generation_vs;
    vs_entry_ = p.vs;
    valid_entry_ = false;
    // Cache: the same VS with the same patched fetches gives the same input.
    const uint64_t key_entry = KeyEntry(p);
    if (key_entry) {
      if (const auto it = inputs_cache_.find(key_entry); it != inputs_cache_.end()) {
        entry_current_ = &it->second;
        valid_entry_ = true;
        cause_entry_ = 0;
        ++inputs_cache_hits_;
        return entry_current_;
      }
    }
    entry_ = VerticesEntry{};
    const uint64_t rejected_before = rejected_;
    const auto before_entry = std::chrono::steady_clock::now();
    const VerticesEntry* entry = ComputeEntry(p);
    ns_inputs_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - before_entry)
                                 .count());
    ++computed_inputs_;
    cause_entry_ = rejected_ != rejected_before ? last_cause_ : 0;
    entry_current_ = &entry_;
    if (entry && key_entry && inputs_cache_.size() < 4096) {
      inputs_cache_.emplace(key_entry, entry_);
    }
    return entry;
  }

  // Key of the input cache: declarations/fetches plus the FULL loaded program.
  // ALU/CF changes can invalidate ordered identity and switch exact selection
  // to the unique-candidate fallback even if every fetch word is unchanged.
  // Seed with the selected VS; 0 = do not cache.
  uint64_t KeyEntry(const SubmissionDraw& p) const {
    const ShaderEntry& vs = *p.vs;
    const auto& patched = p.vs_microcode;
    if (patched.size() != vs.microcode.size() || vs.elements.size() > 32) {
      return 0;
    }
    std::array<uint32_t, 32 * 6> words;
    size_t n = 0;
    for (const ElementVertex& element : vs.elements) {
      const size_t i = size_t(element.instruction) * 3;
      if (i + 2 >= patched.size()) {
        return 0;
      }
      words[n++] = (uint32_t(element.instruction) << 16) | (uint32_t(element.usage) << 8) |
                      element.usage_index;
      words[n++] = vs.microcode[i];
      words[n++] = vs.microcode[i + 1];
      words[n++] = patched[i];
      words[n++] = patched[i + 1];
      words[n++] = patched[i + 2];
    }
    const uint64_t program_hash = XXH3_64bits(patched.data(), patched.size_bytes());
    const uint64_t key = XXH3_64bits_withSeed(
        words.data(), n * sizeof(uint32_t), uint64_t(uintptr_t(p.vs)) ^ program_hash);
    return key ? key : 1;
  }

  const VerticesEntry* ComputeEntry(const SubmissionDraw& p) {
    const ShaderEntry& vs = *p.vs;
    entry_.remaps.fill(kRemapIdentity);
    const auto& patched = p.vs_microcode;
    if (patched.size() != vs.microcode.size()) {
      Reject(20, "the ring's VS does not have the container length");
      return nullptr;
    }
    const bool ordered_identity = me::native::VertexShaderIdentityMatches(
        {me::native::ShaderIdentityStage::Vertex, patched},
        {me::native::ShaderIdentityStage::Vertex, vs.microcode}, vs.elements,
        [](const ElementVertex& e) { return e.instruction; });
    uint32_t used_locations = 0;
    for (const ElementVertex& element : vs.elements) {
      const uint32_t register_value = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
      const uint32_t original = vs.microcode[size_t(element.instruction) * 3 + 1] & 0xFFF;
      const auto selection = me::native::SelectVertexFetch(
          vs.microcode, patched, element.instruction, ordered_identity, vs.elements,
          [](const ElementVertex& e) { return e.instruction; });
      if (!selection) {
        if (selection.failure == me::native::VertexFetchSelectionFailure::Ambiguous) {
          const uint64_t program_hash = XXH3_64bits(patched.data(), patched.size_bytes());
          const uint64_t diagnostic_key = program_hash ^ uint64_t(uintptr_t(p.vs));
          if (me_fetch_ambig_diagnosed_.size() < 16 &&
              me_fetch_ambig_diagnosed_.insert(diagnostic_key).second) {
            REXLOG_WARN("[native] ME ambiguous fetch VS n{} {}{} instr {} temp r{} loaded_hash {:016X} "
                        "words {} (ordered identity false)", vs.number, UsageName(element.usage),
                        element.usage_index, element.instruction, register_value, program_hash, patched.size());
            for (const ElementVertex& e : vs.elements) {
              const size_t i = size_t(e.instruction) * 3;
              if (i + 3 > patched.size()) continue;
              REXLOG_WARN("[native] ME ambiguous VS n{} declared {}{} instr {} selected {:08X} {:08X} {:08X} "
                          "loaded {:08X} {:08X} {:08X}", vs.number, UsageName(e.usage), e.usage_index, e.instruction,
                          vs.microcode[i], vs.microcode[i + 1], vs.microcode[i + 2],
                          patched[i], patched[i + 1], patched[i + 2]);
            }
            uint32_t differences = 0;
            for (size_t i = 0; i < patched.size(); ++i) {
              bool fetch = false;
              for (const ElementVertex& e : vs.elements) {
                const size_t q_decl = size_t(e.instruction) * 3;
                if (i >= q_decl && i - q_decl < 3) { fetch = true; break; }
              }
              if (!fetch && patched[i] != vs.microcode[i] && differences++ < 16)
                REXLOG_WARN("[native] ME ambiguous VS n{} nonfetch word {} loaded {:08X} selected {:08X}",
                            vs.number, i, patched[i], vs.microcode[i]);
            }
            REXLOG_WARN("[native] ME ambiguous VS n{} nonfetch difference count {}", vs.number, differences);
          }
          Reject(21, "ambiguous vertex fetch without ordered VS identity proof");
          return nullptr;
        }
        if (warned_vs_.size() < 16 && warned_vs_.insert(vs.number).second) {
          std::string detail;
          for (const ElementVertex& other : vs.elements) {
            const size_t i = size_t(other.instruction) * 3;
            detail += fmt::format(" {}{}@{}: original r{} op{}, patched r{} op{} {:08X} {:08X} {:08X};",
                                   UsageName(other.usage), other.usage_index, other.instruction,
                                   (vs.microcode[i] >> 12) & 0x3F, vs.microcode[i] & 0x1F,
                                   (patched[i] >> 12) & 0x3F, patched[i] & 0x1F, patched[i],
                                   patched[i + 1], patched[i + 2]);
          }
          REXLOG_WARN("[native] C6 diag: VS n{} without fetch for {}{} (register r{}):{}", vs.number,
                      UsageName(element.usage), element.usage_index, register_value, detail);
        }
        Reject(21, "vertex fetch not found in the patched VS");
        return nullptr;
      }
      const size_t q = selection.word_offset;
      const uint32_t d1 = patched[q + 1], d2 = patched[q + 2];
      const bool mini = ((d1 >> 30) & 0x1) != 0;
      size_t q_base = q;
      if (mini) {
        q_base = SIZE_MAX;
        for (const ElementVertex& other : vs.elements) {
          const size_t i = size_t(other.instruction) * 3;
          if (i < q && (patched[i] & 0x1F) == 0 && !((patched[i + 1] >> 30) & 0x1) &&
              (q_base == SIZE_MAX || i > q_base)) {
            q_base = i;
          }
        }
        if (q_base == SIZE_MAX) {
          Reject(26, "complete base fetch of vfetch_mini not found");
          return nullptr;
        }
      }
      const uint32_t d0_base = patched[q_base], d2_base = patched[q_base + 2];
      const uint32_t slot = ((d0_base >> 20) & 0x1F) * 3 + ((d0_base >> 25) & 0x3);
      const uint32_t format = (d1 >> 16) & 0x3F;
      const uint32_t stride = (d2_base & 0xFF) * 4;
      const int32_t offset = (int32_t(d2 << 1) >> 9) * 4;
      const int32_t location = LocationOfUsage(element.usage, element.usage_index);
      if (location < 0 || ((used_locations >> location) & 0x1)) {
        Warn(22, "vertex element without a free location in the shader: skipped");
        continue;
      }
      const uint32_t code = RemapCode(original, d1 & 0xFFF);
      if (code != kRemapIdentity && warnings_swizzle_ < 24) {
        ++warnings_swizzle_;
        REXLOG_INFO("[native] C6: VS n{} {}{} (format {}): original swizzle {:03X}, patched {:03X}, "
                    "remap {:03X}",
                    vs.number, UsageName(element.usage), element.usage_index, format, original,
                    d1 & 0xFFF, code);
      }
      bool r11g11b10 = false;
      const VkFormat vk = AttributeFormat(format, WholeEntry(element.usage), (d1 >> 12) & 0x1,
                                          (d1 >> 13) & 0x1, false, r11g11b10);
      if (vk == VK_FORMAT_UNDEFINED) {
        Reject(300 + format, "vertex format not supported yet");
        return nullptr;
      }
      if (r11g11b10) {
        entry_.specialization |= 0x1;
      }
      uint32_t binding = 0;
      while (binding < entry_.bindings.size() && entry_.bindings[binding].slot != slot) {
        ++binding;
      }
      if (binding == entry_.bindings.size()) {
        entry_.bindings.push_back({slot, stride});
      } else if (!entry_.bindings[binding].stride) {
        entry_.bindings[binding].stride = stride;
      }
      if (offset < 0) {
        Reject(24, "negative vertex offset");
        return nullptr;
      }
      entry_.attributes.push_back({uint32_t(location), binding, vk, uint32_t(offset)});
      entry_.remaps[location] = REXCVAR_GET(masseffect_native_normalized_remap)
          ? NormalizeRemap(code, r11g11b10 ? 4u : ComponentsVertexFormat(vk)) : code;
      used_locations |= uint32_t(1) << location;
    }
    for (const BindingVertices& binding : entry_.bindings) {
      if (!binding.stride) {
        Reject(25, "vertex stream without stride");
        return nullptr;
      }
    }
    uint64_t fingerprint = XXH3_64bits(entry_.attributes.data(),
                                  entry_.attributes.size() * sizeof(AttributeVertices));
    fingerprint = XXH3_64bits_withSeed(entry_.bindings.data(),
                                  entry_.bindings.size() * sizeof(BindingVertices), fingerprint);
    entry_.fingerprint = fingerprint ^ entry_.specialization;
    valid_entry_ = true;
    return &entry_;
  }

  std::unordered_set<uint32_t> seen_signs_;  // Mass Effect diagnostic (cause 31)

  // Texture of a fetch constant: heap slot and, if it has to be uploaded, it is left in textures_to_upload_
  // with its data already prepared. Base level of 2D textures and cubemaps.
  static uint32_t CapInterval(const Texture& t) {
    uint32_t cap = uint32_t(REXCVAR_GET(masseffect_native_texture_interval_max));
    if (t.late_changes && REXCVAR_GET(masseffect_native_adaptive_texture)) {
      cap = std::min<uint32_t>(cap, t.late_changes >= 3 ? 2u : 4u);
    }
    return cap;
  }

  void PrepareTexture(const uint32_t* f, uint32_t& slot, uint32_t& heap,
                       VkDeviceSize& bytes_upload, bool& punctual_sampling, uint64_t& valid_until,
                       uint32_t& host_width_out, uint32_t& host_height_out) {
    slot = 0;
    heap = 0;
    host_width_out = 0;  // 0 = unknown (1/size is not written)
    host_height_out = 0;
    punctual_sampling = false;
    valid_until = frame_;  // by default, this frame only
    if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
      return;
    }
    const uint32_t dimension = (f[5] >> 9) & 0x3;
    const bool cube = dimension == uint32_t(xenos::DataDimension::kCube);
    const bool volume = dimension == uint32_t(xenos::DataDimension::k3D);
    if (dimension != uint32_t(xenos::DataDimension::k2DOrStacked) && !cube && !volume) {
      Warn(30, "1D textures not supported yet: an empty one is used");
      return;
    }
    heap = cube ? 2 : volume ? 1 : 0;
    const uint32_t layers = cube ? 6 : 1;
    const uint8_t signs = RemappedSigns(f);
    bool signed_value = false;
    for (uint32_t i = 0; i < 4; ++i) {
      signed_value |= ((signs >> (i * 2)) & 0x3) == uint32_t(xenos::TextureSign::kSigned);
    }
    if (signed_value) {
      Warn(31, "signed textures not supported yet: read as unsigned");
      // Mass Effect diagnostic: which formats and sign sets are read unsigned (normal maps?).
      const uint32_t sign_key = (f[1] & 0x3F) | (uint32_t(signs) << 8);
      if (seen_signs_.size() < 48 && seen_signs_.insert(sign_key).second) {
        REXLOG_INFO("[native] C3 sign: format {} signs {:02X} (x{} y{} z{} w{}) swizzle {:03X} base {:08X} {}x{}",
                    f[1] & 0x3F, signs, signs & 3, (signs >> 2) & 3, (signs >> 4) & 3, (signs >> 6) & 3,
                    (f[3] >> 1) & 0xFFF, (f[1] >> 12) << 12, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1);
      }
    }
    const uint32_t swizzle = (f[3] >> 1) & 0xFFF;
    const uint32_t base = (f[1] >> 12) << 12;
    if (!cube && !volume) {
      if (const ImageNative* resolved = context_->ResolvedTexture(base & 0x1FFFFFFF)) {
        AuditFetchResolved(base & 0x1FFFFFFF, f, *resolved);
        resolved = context_->ResolvedTextureForFetch(base & 0x1FFFFFFF, f, *resolved);
        if (!resolved) {
          me_resolved_fetch_failed_this_draw_ = true;
          Reject(109, "resolved logical-size fetch unsupported or GPU copy failed");
          return;
        }
        if (!me::native::IsSingleSample(uint32_t(resolved->sample_count))) {
          me_resolved_fetch_failed_this_draw_ = true;
          Reject(109, "ordinary resolved texture fetch cannot bind a multisample image");
          return;
        }
        // Without this the shadows flicker.
        // With masseffect_native_inv_tex_size the shader takes 1/size from the shared constants, and this path
        // used to return without reporting the size: 0 was written and the tfetch offsets (the taps of the
        // shadow map's PCF filter) all landed on the same texel. The filtering disappeared and the edge
        // shimmered as the camera moved. Resolved textures report their size like any other.
        host_width_out = resolved->width;
        host_height_out = resolved->height;
        // One trace per size, to check in the log that the 1/size constant of resolved textures is no longer
        // 0 (that was the cause of the shadow flicker).
        if (warnings_inv_resolved_size_.insert(uint64_t(resolved->width) << 32 | resolved->height).second) {
          REXLOG_INFO("[native] C3: resolved texture {}x{}: 1/size = {:.6f}, {:.6f}",
                      resolved->width, resolved->height, 1.0f / float(resolved->width),
                      1.0f / float(resolved->height));
        }
        if (IsDepth(resolved->format) || resolved->resolved_depth_guestspace) {
          // Depth copy (k_24_8): depth comes out in R and the fetch constant's swizzle distributes it. No
          // filtering: it is read as is.
          slot = SlotView(resolved->image, resolved->format, swizzle, kSwizzleRRRR);
          punctual_sampling = true;
          // This used to be UINT64_MAX, and that is what forced the cross-frame cache off.
          // A resolved texture is a render target the game rewrites every frame. Saying its descriptor slot is
          // valid "until the generation changes" meant that, with the CPU two frames ahead (3 work slots), an
          // old view was reused: that was a smeared text overlay that kept
          // masseffect_native_cache_textures_between_frames at false for a while. The other paths are already tied
          // to texture.next and relax nothing; these two were the only ones without a horizon.
          valid_until = frame_;
          return;
        }
        // The emulation writes the copy to memory with copy_dest_swap and loads it as a texture: the fetch
        // constant's swizzle (ZYXW for 8888) undoes that swap. Here the copy is image to image and does not
        // swap channels, so the swap goes into the host channels (without this, Mia came out blue).
        slot = SlotView(resolved->image, resolved->format, swizzle,
                             resolved->swap_rb ? kSwizzleBGRA : kSwizzleRGBA);
        valid_until = frame_;  // see the long comment on the depth copy
        return;
      }
    }
    const uint32_t format = f[1] & 0x3F;
    TextureFormat tf;
    if (!TextureFormatFor(format, tf)) {
      Warn(400 + format, "texture format not supported yet: an empty one is used");
      return;
    }
    // size_2d: 13 + 13 bits; size_3d: 11 + 11 + 10 bits (xenos.h:1222-1233).
    const uint32_t width = volume ? (f[2] & 0x7FF) + 1 : (f[2] & 0x1FFF) + 1;
    const uint32_t height = volume ? ((f[2] >> 11) & 0x7FF) + 1 : ((f[2] >> 13) & 0x1FFF) + 1;
    const uint32_t background = volume ? ((f[2] >> 22) & 0x3FF) + 1 : 0;
    if (volume && tf.block > 1) {
      Warn(38, "compressed 3D texture: an empty one is used");
      return;
    }
    const uint32_t blocks_x = (width + tf.block - 1) / tf.block;
    const uint32_t blocks_y = (height + tf.block - 1) / tf.block;
    const uint32_t pitch_blocks = std::max<uint32_t>((((f[0] >> 22) & 0x1FF) << 5) / tf.block, 1);
    // The faces of a cubemap are consecutive: each takes its base level with rows and columns aligned to
    // 32 blocks and the total to 4 KB (GetGuestTextureLayout).
    const uint64_t stride_face =
        (uint64_t((pitch_blocks + 31) & ~uint32_t(31)) * tf.bytes *
             ((blocks_y + 31) & ~uint32_t(31)) +
         4095) &
        ~uint64_t(4095);
    const uint32_t keys[5] = {f[0] & 0xFFC003FC, f[1], f[2], (f[4] >> 2) & 0xFF, f[5] >> 9};
    const uint64_t key = XXH3_64bits(keys, sizeof(keys));

    // Mip levels with the rules of GetSubresourcesFromFetchConstant: no
    // mip address means no mips; the maximum is clamped to the size; if the base is missing or the minimum
    // is above 0, the base is not read. 3D textures still use only the base.
    const bool tile_texture = (f[0] >> 31) & 0x1;
    const uint64_t dir_base = uint64_t(base) & 0x1FFFFFFF;
    const uint64_t dir_mips = uint64_t((f[5] >> 12) & 0x1FFFF) << 12;
    const uint32_t packed_level =
        mipmaps_ && !volume && ((f[5] >> 11) & 0x1) ? PackedLevel(width, height) : UINT32_MAX;
    uint32_t level_max = 0;
    bool read_base = true;
    if (mipmaps_ && !volume && dir_mips != 0) {
      const uint32_t size_max = Log2Floor(std::max(width, height));
      uint32_t level_min = std::min((f[4] >> 2) & 0xFu, size_max);
      level_max = std::max(std::min((f[4] >> 6) & 0xFu, size_max), level_min);
      if (level_max != 0) {
        if (dir_base == 0) {
          level_min = std::max(level_min, 1u);
        }
        read_base = level_min == 0;
      }
    }
    // Mip regions from dir_mips (GetGuestTextureLayout): each level stored with rows of 32 blocks computed
    // from the size rounded up to a power of 2 (and aligned to 256 bytes if the texture is linear) and
    // layers aligned to 4 KB; the levels of the packed tail share the region of the first of them.
    struct RegionMip {
      uint64_t displacement = 0;
      uint64_t row_bytes = 0;
      uint64_t stride = 0;
      uint32_t pitch_blocks = 0;
    };
    std::array<RegionMip, 16> regions{};
    uint64_t extension_mips = 0;
    if (level_max != 0) {
      const uint32_t last = packed_level == 0 ? 0 : std::min(level_max, packed_level);
      for (uint32_t s = packed_level == 0 ? 0 : 1; s <= last; ++s) {
        RegionMip& region = regions[s];
        const uint32_t row_texels = std::max(std::bit_ceil(width) >> s, 1u);
        const uint32_t rows_texels = std::max(std::bit_ceil(height) >> s, 1u);
        region.pitch_blocks = ((row_texels + tf.block - 1) / tf.block + 31) & ~31u;
        region.row_bytes = uint64_t(region.pitch_blocks) * tf.bytes;
        if (!tile_texture) {
          region.row_bytes = (region.row_bytes + 255) & ~uint64_t(255);
        }
        const uint64_t rows_blocks = uint64_t(((rows_texels + tf.block - 1) / tf.block + 31) & ~31u);
        region.stride = (region.row_bytes * rows_blocks + 4095) & ~uint64_t(4095);
        region.displacement = extension_mips;
        extension_mips += region.stride * layers;
      }
      if (dir_mips + extension_mips > kPhysicalMemory) {
        level_max = 0;  // the mips would go past memory: base only
        extension_mips = 0;
        read_base = true;
      }
    }
    // With the packed tail starting at level 0 (short side of 16 texels or less), the base does not start
    // at its address either (VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl applies the same
    // offset to level 0).
    uint32_t base_ox = 0;
    uint32_t base_oy = 0;
    if (packed_level == 0) {
      PackedDisplacement(width, height, tf.block, 0, base_ox, base_oy);
    }

    Texture& texture = textures_[key];
    if (texture.image.image == VK_NULL_HANDLE) {
      // BC in Vulkan: the size is in whole blocks.
      const uint32_t host_width = tf.block > 1 ? (width + 3) & ~uint32_t(3) : width;
      const uint32_t host_height = tf.block > 1 ? (height + 3) & ~uint32_t(3) : height;
      // With its mip levels (those that fit in the host size).
      const uint32_t levels = std::min(level_max + 1, Log2Floor(std::max(host_width, host_height)) + 1);
      /*
       * The vkBindImageMemory goes to the bind thread if possible (masseffect_native_textures_binding_thread) and,
       * otherwise, CreateTexture as usual. What the ring spends creating is timed, without its wait for the
       * thread (that is counted separately), for the [hitch] ring line and the 10 s report.
       */
      const auto before_create = std::chrono::steady_clock::now();
      const uint64_t wait_before_create = ns_wait_bindings_total_;
      measuring_creation_ = true;
      const bool created = CreateTextureInThread(texture, tf.format, host_width, host_height, layers, background, levels) ||
                          CreateTexture(texture.image, tf.format, host_width, host_height, layers, background, levels);
      measuring_creation_ = false;
      {
        const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - before_create)
                                         .count());
        const uint64_t wait = ns_wait_bindings_total_ - wait_before_create;
        const uint64_t own = ns > wait ? ns - wait : 0;
        masseffect::waits::g_ns_create_textures.fetch_add(own, std::memory_order_relaxed);
        ns_create_report_ += own;
      }
      if (!created) {
        textures_.erase(key);
        Warn(32, "could not create a texture");
        return;
      }
      texture.layers = layers;
      texture.background = background;
      texture.levels = levels;
      // Measurement only: the shape without the address, to count, once its first hash is known, whether it
      // repeats the content of a live one (NoteContentTexture). If this image is being recreated, the
      // previous one stops counting. The words are the key's without the base and mip addresses and without
      // bit 11 of f[1], which the sampler sets: format, byte order, tiling, pitch, size, mip limits, dimension
      // and packed tail.
      if (DiagReuse()) {
        RemoveContentTexture(texture, key);
        const uint32_t shape[5] = {keys[0], f[1] & 0x7FF, keys[2], keys[3], (f[5] >> 9) & 0x7};
        texture.shape_content = XXH3_64bits(shape, sizeof(shape));
        texture.address = base;
        texture.content_per_measure = true;
      }
      for (uint32_t n = 0; n < levels; ++n) {
        texture.bytes += uint64_t((std::max(host_width >> n, 1u) + tf.block - 1) / tf.block) *
                         ((std::max(host_height >> n, 1u) + tf.block - 1) / tf.block) * tf.bytes * layers *
                         (background ? background : 1);
      }
      bytes_textures_ += texture.bytes;
      // Diagnostic: why the cache grows on the console. A new texture at an address that already had another
      // points to world zones reloaded at the same place; with the same address, format and size but a
      // different key, to key fields that change (mip limits, byte order...).
      {
        ++created_textures_;
        masseffect::waits::g_created_textures.fetch_add(1, std::memory_order_relaxed);
        const uint64_t shape = XXH3_64bits_withSeed(&base, sizeof(base), (uint64_t(format) << 40) ^
                                                                              (uint64_t(width) << 20) ^ height);
        if (created_per_address_[base]++ > 0) {
          ++created_in_view_address_;
        }
        const auto [it_shape, new_shape] = key_per_shape_.try_emplace(shape, key);
        if (!new_shape && it_shape->second != key) {
          ++created_same_shape_other_key_;
          it_shape->second = key;
          /*
           * In the main menu, ~100 textures per frame once came back with the same address, format and size and
           * a different key, and were created again every frame. This shows which key words change (the first
           * 40 times).
           */
          const auto [it_words, new_entries] = words_per_shape_.try_emplace(shape);
          /*
           * Key guard. The same five words cannot produce a different key: if one does, the key was not computed
           * from what its words say (as when XXH3 read keys[4] before it was written).
           */
          if (!new_entries && std::equal(std::begin(keys), std::end(keys), it_words->second.begin())) {
            const uint64_t times = ++incoherent_keys_;
            if (times == 1 || times == 10 || times == 100 || times == 1000 || times % 10000 == 0) {
              REXLOG_ERROR("[native] C3 INCONSISTENT texture key ({} times): {:08X} {}x{} format {} with the "
                           "same five words as the previous time and a different key; the key is not derived from its words",
                           times, base, width, height, format);
            }
          }
          if (!new_entries && warnings_other_key_ < 40) {
            ++warnings_other_key_;
            const auto& before = it_words->second;
            REXLOG_INFO("[native] C3 same texture with another key: {:08X} {}x{} format {} | before {:08X} {:08X} "
                        "{:08X} {:02X} {:06X} | now {:08X} {:08X} {:08X} {:02X} {:06X} | changed {:08X} {:08X} "
                        "{:08X} {:02X} {:06X}",
                        base, width, height, format, before[0], before[1], before[2], before[3], before[4], keys[0],
                        keys[1], keys[2], keys[3], keys[4], before[0] ^ keys[0], before[1] ^ keys[1],
                        before[2] ^ keys[2], before[3] ^ keys[3], before[4] ^ keys[4]);
          }
          std::copy(std::begin(keys), std::end(keys), it_words->second.begin());
        } else if (new_shape) {
          auto& words = words_per_shape_[shape];
          std::copy(std::begin(keys), std::end(keys), words.begin());
        }
        // From 256 to 1024. This line and the GPU memory line after it were the two longest stutters of the
        // "diagnostic dump" group: 147.6 ms and 107.5 ms of frame time on their own. With 1024 they
        // still appear once or twice per session, which is enough to see whether the duplicate key counter
        // spikes.
        if (created_textures_ % 1024 == 0) {
          REXLOG_INFO("[native] C3 cache diag: {} textures created ({} in the cache, {} MB): {} at an address that already "
                      "had another texture; {} with the same address, format and size as another one but a different key",
                      created_textures_, textures_.size(), bytes_textures_ >> 20, created_in_view_address_,
                      created_same_shape_other_key_);
          NoteMemoryOfTheGpu();
        }
      }
      if (textures_.size() <= 48) {
        REXLOG_INFO("[native] C3: texture {:08X} {}x{} format {} {} order {} pitch {} "
                    "swizzle {:03X} signs {:02X}{}; levels {} (mips at {:08X}, packed from {})",
                    base, width, height, format, ((f[0] >> 31) & 0x1) ? "tiled" : "linear",
                    (f[1] >> 6) & 0x3, ((f[0] >> 22) & 0x1FF) << 5, swizzle, (f[0] >> 2) & 0xFF,
                    cube ? " (cube)" : volume ? " (3D)" : "", levels, dir_mips,
                    packed_level == UINT32_MAX ? std::string("none") : std::to_string(packed_level));
        if (format == 2 && !tile_texture && width >= 640 && height >= 360) {
          const uint8_t* flat = memory_->TranslatePhysical(base & 0x1FFFFFFF);
          uint8_t min = 255, max = 0;
          uint64_t sum = 0;
          for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* row = flat + uint64_t(y) * pitch_blocks;
            for (uint32_t x = 0; x < width; ++x) {
              min = std::min(min, row[x]);
              max = std::max(max, row[x]);
              sum += row[x];
            }
          }
          REXLOG_INFO("[native] ME video plane {:08X}: min {} max {} mean {:.1f}, first "
                      "{:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
                      base, min, max, double(sum) / double(uint64_t(width) * height), flat[0], flat[1],
                      flat[2], flat[3], flat[4], flat[5], flat[6], flat[7]);
        }
      }
    }
    slot = SlotView(texture.image.image, tf.format, swizzle, tf.swizzle_host, heap);
    host_width_out = texture.image.width;  // the host's, which is what the shader sees
    host_height_out = texture.image.height;
    if (texture.frame == frame_) {
      // Valid until the frame before the next check (if it changed now, only this frame)
      valid_until = std::max<uint64_t>(frame_, texture.next ? texture.next - 1 : 0);
      return;  // already checked this frame
    }
    // Textures that do not change are checked less and less often (down to every 32 frames); those that
    // change (videos) go back to being checked every frame.
    if (texture.image.prepared && frame_ < texture.next) {
      valid_until = texture.next - 1;
      return;
    }
    texture.frame = frame_;
    // What it costs from here to the end (check, untile, prepare the upload).
    struct StopwatchTexture {
      std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
      ~StopwatchTexture() {
        masseffect::waits::g_ns_textures.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                         .count()),
            std::memory_order_relaxed);
      }
    } stopwatch_texture;
    // 2D and cubemaps: before untiling, XXH3 of the guest bytes. If they have not changed there is nothing
    // to do (untiling and comparing every texture every frame took 62 of the 66 us of each menu draw).
    if (!volume) {
      const uint32_t log2_block = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                                : tf.bytes >= 2 ? 1 : 0;
      // Extent of a layer: tiled, GetTiledAddressUpperBound2D; linear,
      // up to the last block. Includes the packed base offset (base_ox, base_oy; 0 for the rest).
      const uint32_t blocks_x_read = blocks_x + base_ox;
      const uint32_t blocks_and_read = blocks_y + base_oy;
      const uint64_t extension_layer =
          ((f[0] >> 31) & 0x1)
              ? uint64_t(std::max<int64_t>(
                    TileDisplacement2D(int32_t((blocks_x_read - 1) & ~31u),
                                            int32_t((blocks_and_read - 1) & ~31u), pitch_blocks,
                                            log2_block),
                    0)) +
                    (log2_block == 0   ? 0xA00u
                     : log2_block == 1 ? 0xC00u
                                        : (0x400u << log2_block))
              : uint64_t(std::max(pitch_blocks, blocks_x_read)) * tf.bytes * (blocks_and_read - 1) +
                    uint64_t(blocks_x_read) * tf.bytes;
      const uint64_t raw_start = uint64_t(base) & 0x1FFFFFFF;
      const uint64_t extension = (layers - 1) * stride_face + extension_layer;
      if (raw_start + extension <= kPhysicalMemory) {
        /*
         * Per-frame check budget.
         * During stutters both game threads wait for room in the ring (20-50 ms) while the ring neither
         * stops nor waits for the GPU: the ring thread itself is stuck. During the stutters, that thread was in
         * PrepareTexture and in XXH3. Textures that arrive together (one zone) double their interval at the
         * same time (1, 2, 4... 32), so all of them are rechecked in the same frame. Here a stable texture
         * (interval 8 or more) whose check does not fit in the frame's budget is deferred a few frames, at
         * most kPostponementMax in a row. New textures, changing ones (videos) and just-uploaded ones are
         * never deferred.
         */
        /*
         * Sampled recheck of stable textures (masseffect_native_fingerprints_sampling).
         * A stable texture used to go through XXH3 in full every 32-39 frames only to conclude, almost always,
         * that it had not changed: in a busy scene that was 0.2-5.3 ms per frame of the ring thread. Now every
         * full check from interval 4 on also computes the hash of a sample of the same memory (SampleFingerprint:
         * first and last 4 KB block and 1 in 8, of the base and the mips) and, if the full hash matches, stores
         * it. Rechecks of stable textures compute only the sample: if it matches, the texture is taken as
         * unchanged; otherwise the usual full path follows. Textures where the sample would read more than half
         * the bytes (those with few blocks) work as before.
         * Self-checking guard: the first kSamplesToCheck rechecks of stable textures compute both hashes
         * and count disagreements (same sample, different full hash). After that, 1 in N rechecks of each
         * texture is still full (N = the cvar), so a change the sample misses is caught at most N rechecks
         * later. A single disagreement turns sampling off for the rest of the session, with a warning.
         */
        if (sampling_fingerprints_ < 0) {
          const int32_t each = REXCVAR_GET(masseffect_native_fingerprints_sampling);
          sampling_fingerprints_ = each >= 2 ? std::min<int32_t>(each, 64) : 0;
          REXLOG_INFO("[native] C3: sampled recheck of stable textures (masseffect_native_fingerprints_sampling) "
                      "= {}",
                      sampling_fingerprints_ ? fmt::format("YES, 1 in {} full; the first {} with both fingerprints",
                                                      sampling_fingerprints_, kSamplesToCheck)
                                        : std::string("no, always the full fingerprint"));
        }
        const uint64_t bytes_complete = extension + extension_mips;
        const uint64_t bytes_of_sample = BytesSample(extension) + BytesSample(extension_mips);
        const bool with_sample = sampling_fingerprints_ > 0 && texture.image.prepared && texture.interval >= 4 &&
                                 bytes_of_sample * 2 <= bytes_complete;
        const bool sample_util = with_sample && texture.interval >= 8 && texture.valid_sample;
        const bool sample_only = sample_util && checked_samples_ >= kSamplesToCheck &&
                                  texture.consecutive_samples + 1u < uint32_t(sampling_fingerprints_);
        const uint64_t bytes_sample = with_sample ? bytes_of_sample : 0;
        // What will actually be read, which is what counts against the budget.
        const uint64_t bytes_fingerprint = sample_only ? bytes_sample : bytes_complete + bytes_sample;
        if (frame_fingerprints_ != frame_) {
          frame_fingerprints_ = frame_;
          bytes_frame_fingerprint_ = 0;
        }
        if (budget_fingerprints_ < 0) {
          budget_fingerprints_ = std::max(REXCVAR_GET(masseffect_native_fingerprints_kb_frame), 0);
          REXLOG_INFO("[native] C3: texture recheck budget = {} KB per frame{}",
                      budget_fingerprints_, budget_fingerprints_ ? "" : " (no limit)");
        }
        if (budget_fingerprints_ && texture.image.prepared && texture.interval >= 8 &&
            texture.postponements < kPostponementMax && bytes_frame_fingerprint_ > 0 &&
            bytes_frame_fingerprint_ + bytes_fingerprint > uint64_t(budget_fingerprints_) * 1024) {
          ++texture.postponements;
          texture.next = frame_ + 1 + (key & 1);
          valid_until = texture.next - 1;
          masseffect::waits::g_postponed_fingerprints.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        texture.postponements = 0;
        bytes_frame_fingerprint_ += bytes_fingerprint;
        masseffect::waits::g_bytes_fingerprint.fetch_add(bytes_fingerprint, std::memory_order_relaxed);
        const auto start_raw_fingerprint = std::chrono::steady_clock::now();
        const uint8_t* const raw_base = memory_->TranslatePhysical(uint32_t(raw_start));
        const uint8_t* const raw_mips = extension_mips ? memory_->TranslatePhysical(uint32_t(dir_mips)) : nullptr;
        // The sample goes before the full hash. If the full one matches, the sample is of that same content,
        // and it is the one stored.
        uint64_t sample = 0;
        if (with_sample) {
          sample = SampleFingerprint(raw_base, extension, 0);
          if (extension_mips) {
            sample = SampleFingerprint(raw_mips, extension_mips, sample);
          }
          bytes_sample_ += bytes_sample;
          ReportSampling(start_raw_fingerprint);
        }
        const bool equal_sample = sample_util && sample == texture.sample_fingerprint;
        if (sample_only && equal_sample) {
          masseffect::waits::g_ns_raw_fingerprint.fetch_add(
              uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                            start_raw_fingerprint)
                           .count()),
              std::memory_order_relaxed);
          ++texture.consecutive_samples;
          ++samples_hits_;
          bytes_saved_sample_ += bytes_complete - bytes_sample;
          texture.interval = std::min<uint32_t>(texture.interval * 2, CapInterval(texture));
          texture.next = frame_ + texture.interval + (texture.interval >= 32 ? (key >> 7) & 7 : 0);
          valid_until = texture.next - 1;
          return;
        }
        if (sample_only) {
          // The sample changed: now the full hash, which was not in this frame's budget.
          ++distinct_samples_;
          bytes_frame_fingerprint_ += bytes_complete;
          masseffect::waits::g_bytes_fingerprint.fetch_add(bytes_complete, std::memory_order_relaxed);
        }
        uint64_t raw_fingerprint = XXH3_64bits(raw_base, size_t(extension));
        if (extension_mips) {  // and the bytes of all the mips
          raw_fingerprint = XXH3_64bits_withSeed(raw_mips, size_t(extension_mips), raw_fingerprint);
        }
        masseffect::waits::g_ns_raw_fingerprint.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                          start_raw_fingerprint)
                         .count()),
            std::memory_order_relaxed);
        const bool complete_equal = texture.image.prepared && raw_fingerprint == texture.raw_fingerprint;
        if (sample_util && !sample_only) {
          // The guard. This stable recheck computed both hashes.
          ++checked_samples_;
          ++samples_with_the_two_;
          if (equal_sample && !complete_equal) {
            REXLOG_ERROR("[native] C3: the SAMPLE of texture {:08X} {}x{} format {} ({} KB base and {} KB of "
                         "mips) did not see a change that the full fingerprint did. Fingerprint sampling is SWITCHED OFF "
                         "(after {} rechecks with both).",
                         uint32_t(raw_start), width, height, format, extension >> 10, extension_mips >> 10,
                         checked_samples_);
            sampling_fingerprints_ = 0;
          } else if (checked_samples_ == kSamplesToCheck) {
            REXLOG_INFO("[native] C3: fingerprint sampling: {} rechecks with both fingerprints, no "
                        "disagreement. Sampling continues, with 1 in {} full.",
                        checked_samples_, sampling_fingerprints_);
          }
        }
        if (complete_equal) {
          // The sample of this content, for the stable rechecks.
          if (with_sample && sampling_fingerprints_ > 0) {
            texture.sample_fingerprint = sample;
            texture.valid_sample = true;
          }
          texture.consecutive_samples = 0;
          texture.interval = std::min<uint32_t>(texture.interval * 2, CapInterval(texture));
          // At the maximum interval, from 32 to 39 depending on the texture, so they do not coincide.
          texture.next = frame_ + texture.interval + (texture.interval >= 32 ? (key >> 7) & 7 : 0);
          valid_until = texture.next - 1;
          return;
        }
        texture.valid_sample = false;  // the content changed; the sample is no longer valid
        if (texture.image.prepared && texture.interval >= 2 && texture.late_changes < 255) {
          ++texture.late_changes;
        }
        if (texture.image.prepared && texture.interval >= 4) {
          // Diagnostic (texture popping): a stable texture changed at the same address; the old content was
          // shown for up to `interval` frames.
          static uint32_t late_changes = 0;
          ++late_changes;
          if (late_changes <= 64 || (late_changes & 1023) == 0)
            REXLOG_INFO("[native] ME stable texture changed at {:08X} {}x{} format {} after interval {} (maximum "
                        "delay {} frames; late changes {}, sample {})",
                        uint32_t(raw_start), width, height, format, texture.interval, texture.interval,
                        late_changes, sample_only ? "sample only" : "full");
        }
        texture.consecutive_samples = 0;
        texture.raw_fingerprint = raw_fingerprint;
        NoteContentTexture(texture, key);  // measurement only: masseffect_native_diag_reuse
      }
    }
    // Base level of each layer, untiled and in host byte order.
    const uint32_t blocks_x_host = (texture.image.width + tf.block - 1) / tf.block;
    const uint32_t blocks_y_host = (texture.image.height + tf.block - 1) / tf.block;
    const size_t bytes_layer = size_t(blocks_x_host) * blocks_y_host * tf.bytes;
    std::vector<uint8_t>& data = temporal_;
    // All levels back to back, each one layer after layer (UploadTexture copies them one by one).
    std::array<size_t, 16> bytes_layer_level{};
    size_t bytes_data = 0;
    for (uint32_t n = 0; n < texture.levels; ++n) {
      const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
      texture.level_displacement[n] = uint32_t(bytes_data);
      bytes_layer_level[n] = size_t(bx_host) * by_host * tf.bytes;
      bytes_data += bytes_layer_level[n] * layers * (background ? background : 1);
    }
    data.assign(bytes_data, 0);
    const bool tile = (f[0] >> 31) & 0x1;
    if (volume && !ReadVolume(f, tf, blocks_x, blocks_y, background, blocks_x_host,
                                blocks_y_host, pitch_blocks, tile, data)) {
      Warn(33, "texture outside memory");
      if (!texture.image.prepared) {
        slot = 0;  // the image remains uninitialized
      }
      return;
    }
    // Base level: not read if not needed (minimum level 1; it stays zeroed and the sampler never goes below
    // 1).
    for (uint32_t c = 0; c < (volume || !read_base ? 0u : layers); ++c) {
      const uint64_t address = dir_base + c * stride_face;
      if (!ReadLevel(address, tile, pitch_blocks, uint64_t(std::max(pitch_blocks, blocks_x)) * tf.bytes, tf,
                     base_ox, base_oy, blocks_x, blocks_y, blocks_x_host, data.data() + bytes_layer * c)) {
        Warn(33, "texture outside memory");
        if (!texture.image.prepared) {
          slot = 0;  // the image remains uninitialized
        }
        return;
      }
    }
    // Mip levels. Those in the packed tail share a region and are located with PackedDisplacement;
    // the rest, each in its own.
    for (uint32_t n = 1; n < texture.levels; ++n) {
      const uint32_t s = packed_level == 0 ? 0 : std::min(n, packed_level);
      uint32_t ox = 0;
      uint32_t oy = 0;
      if (n >= packed_level) {
        PackedDisplacement(width, height, tf.block, n, ox, oy);
      }
      const uint32_t bx_guest = (std::max(width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_guest = (std::max(height >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t bx_host = (std::max(texture.image.width >> n, 1u) + tf.block - 1) / tf.block;
      const uint32_t by_host = (std::max(texture.image.height >> n, 1u) + tf.block - 1) / tf.block;
      for (uint32_t c = 0; c < layers; ++c) {
        const uint64_t address = dir_mips + regions[s].displacement + c * regions[s].stride;
        if (!ReadLevel(address, tile, regions[s].pitch_blocks, regions[s].row_bytes, tf, ox, oy,
                       std::min(bx_guest, bx_host), std::min(by_guest, by_host), bx_host,
                       data.data() + texture.level_displacement[n] + bytes_layer_level[n] * c)) {
          Warn(39, "mip level outside memory: left at zero");
          break;
        }
      }
    }
    const auto order = static_cast<xenos::Endian>((f[1] >> 6) & 0x3);
    // In one go (ChangeOrderBytes), with the same guard as the fast untiling.
    if (tile_fast_ > 0 && order != xenos::Endian::kNone && (tf.unit_order == 2 || tf.unit_order == 4)) {
      // The first 200, then 1 in 64. Checking every texture, this guard never got to switch itself off (there
      // were only 1,024 textures) and cost 2.1 ms per MB on every new texture.
      ++seen_orders_;
      const bool check = seen_orders_ <= kLevelsToCheck || seen_orders_ % kCheckAOfEach == 0;
      if (check) {
        check_tile_ = data;
      }
      ChangeOrderBytes(data.data(), data.size(), tf.unit_order, uint32_t(order));
      if (check) {
        ++checked_orders_;
        OrderOfAlways(check_tile_, tf.unit_order, order);
        if (check_tile_ != data) {
          REXLOG_ERROR("[native] C3: the fast order change does NOT match (unit {}, order {}, {} bytes). The "
                       "fast order and untiling are SWITCHED OFF.",
                       tf.unit_order, uint32_t(order), data.size());
          data.swap(check_tile_);
          tile_fast_ = 0;
        } else if (checked_orders_ == kLevelsToCheck) {
          REXLOG_INFO("[native] C3: fast order change: {} textures checked against the regular path, all "
                      "equal.",
                      checked_orders_);
        }
      }
    } else {
      OrderOfAlways(data, tf.unit_order, order);
    }
    /*
     * For a new (unprepared) texture this hash decides nothing: it is uploaded regardless. 0 is stored, and
     * the first time its memory changes it will be computed and uploaded (at most, one extra upload).
     */
    const auto start_data_fingerprint = std::chrono::steady_clock::now();
    const uint64_t fingerprint = texture.image.prepared ? XXH3_64bits(data.data(), data.size()) : 0;
    masseffect::waits::g_ns_data_fingerprint.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                      start_data_fingerprint)
                     .count()),
        std::memory_order_relaxed);
    if (texture.image.prepared && fingerprint == texture.fingerprint) {
      texture.interval = std::min<uint32_t>(texture.interval * 2, CapInterval(texture));
      texture.next = frame_ + texture.interval;
      valid_until = texture.next - 1;
      return;
    }
    texture.interval = 1;
    texture.next = frame_ + 1;
    texture.fingerprint = fingerprint;
    texture.data.swap(data);
    texture.needs_upload = true;
    textures_to_upload_.push_back(&texture);
    masseffect::waits::g_textures_uploads.fetch_add(1, std::memory_order_relaxed);
    masseffect::waits::g_bytes_uploaded.fetch_add(texture.data.size(), std::memory_order_relaxed);
    bytes_upload += (texture.data.size() + 3) & ~size_t(3);
  }

  // --- Measurement only: new textures with the content of another (masseffect_native_diag_reuse) ---------------
  /*
   * What it is for. The game reloads the zone packs at new addresses every time it returns to a zone (in
   * the cache diagnostic, of 1,024 textures created, only 6 landed at an address already used). Since the
   * address is part of the key, a returning texture is a new texture: the image is created, bound,
   * untiled and uploaded again, even though the same image, with the same bytes, is still in the cache
   * under the old key and nothing uses it. This measures how often that happens:
   *   - how many new textures have the same content and shape as a live one (the image would be the same);
   *   - how many of those others are cold: not checked for more than kFramesNoUsageForDrop frames,
   *     which is what eviction uses to mean "unused" (a texture in use is checked at least every 39
   *     frames). Those are the duplicates the cache holds that nothing in use depends on.
   * With MB, to see how much GPU memory and upload traffic is behind them. It changes nothing: no
   * decision of the ring reads what is recorded here.
   *
   * How. live_per_content_ holds every texture with a valid raw hash under its content key: the XXH3
   * of its shape without the address (the key words without the base and mip addresses, plus the
   * VkFormat, the host size, the levels and the layers) seeded with its raw hash (the guest bytes of base
   * and mips). Two textures with the same content key have the same bytes read with the same layout: the
   * same host data. It is maintained:
   *   - when the image is created (the previous one is removed if the image is recreated);
   *   - when the raw hash is set: the usual path of PrepareTexture;
   *   - when the texture is released: DropImages, which the gradual, batch and out-of-memory
   *     evictions all go through.
   * A new texture, once its first hash is known, looks at those with the same content key, at most
   * kReuseLookMax: O(1) per texture, never a walk over the live ones. Removing one is O(k), with k
   * the ones with the same content (the repeats; almost always 0 or 1). Candidates are validated when
   * looked at (still under that content key, with a prepared image), so a
   * stale entry cannot count.
   */
  static constexpr uint32_t kReuseLookMax = 8;  // candidates checked per new texture
  static constexpr uint32_t kReuseDetails = 8;  // lines detailing the first matches

  bool DiagReuse() {
    if (diag_reuse_ < 0) {
      diag_reuse_ = REXCVAR_GET(masseffect_native_diag_reuse) ? 1 : 0;
      REXLOG_INFO("[native] C3: measurement of new textures with the content of another live one "
                  "(masseffect_native_diag_reuse) = {}",
                  diag_reuse_ ? "YES (counting only: changes nothing)" : "no");
    }
    return diag_reuse_ > 0;
  }

  // The content key of a texture with a valid raw hash. Never 0 (0 = not recorded).
  static uint64_t ContentKey(const Texture& texture) {
    const uint64_t shape[4] = {texture.shape_content, (uint64_t(texture.image.format) << 32) | texture.levels,
                               (uint64_t(texture.image.width) << 32) | texture.image.height, texture.layers};
    return XXH3_64bits_withSeed(shape, sizeof(shape), texture.raw_fingerprint) | 1;
  }

  // Stops counting a texture (when it is released, when its image is recreated, or before recording it
  // with another hash).
  void RemoveContentTexture(Texture& texture, uint64_t key) {
    if (!texture.content_key) {
      return;
    }
    const auto [from, until] = live_per_content_.equal_range(texture.content_key);
    for (auto it = from; it != until; ++it) {
      if (it->second == key) {
        live_per_content_.erase(it);
        break;
      }
    }
    if (live_per_content_.find(texture.content_key) == live_per_content_.end() && distinct_contents_) {
      --distinct_contents_;
    }
    texture.content_key = 0;
  }

  // With the raw hash just set. If the texture is new, counts whether another live one has the same
  // content and shape, and whether that other one is cold. Then records it under its content key. Ring
  // thread only.
  void NoteContentTexture(Texture& texture, uint64_t key) {
    if (!DiagReuse()) {
      return;
    }
    RemoveContentTexture(texture, key);
    const uint64_t content = ContentKey(texture);
    if (texture.content_per_measure) {
      texture.content_per_measure = false;
      ++reuse_new_;
      reuse_bytes_new_ += texture.bytes;
      const Texture* equal = nullptr;
      const Texture* cold = nullptr;
      uint32_t looked = 0;
      const auto [from, until] = live_per_content_.equal_range(content);
      for (auto it = from; it != until; ++it) {
        if (looked++ >= kReuseLookMax) {
          ++reuse_no_look_;
          break;
        }
        const auto other = textures_.find(it->second);
        if (other == textures_.end() || &other->second == &texture) {
          continue;
        }
        const Texture& t = other->second;
        if (t.content_key != content || t.image.image == VK_NULL_HANDLE || !t.image.prepared) {
          continue;
        }
        equal = &t;
        if (t.frame != UINT64_MAX && t.frame + kFramesNoUsageForDrop < frame_) {
          cold = &t;
          break;  // one cold one is enough
        }
      }
      if (equal) {
        ++reuse_match_;
        reuse_bytes_match_ += texture.bytes;
      }
      if (cold) {
        ++reuse_cold_;
        reuse_bytes_cold_ += texture.bytes;
      }
      if (equal && reuse_details_ < kReuseDetails) {
        ++reuse_details_;
        const Texture& t = cold ? *cold : *equal;
        MASSEFFECT_REPORT_RING("[native] C3 reuse by content (measurement only): new texture {:08X} "
                             "({}x{}, VkFormat {}, {} levels, {} layers, {} KB) has the same content and shape as "
                             "{:08X}, {}",
                             texture.address, texture.image.width, texture.image.height,
                             uint32_t(texture.image.format), texture.levels, texture.layers, texture.bytes >> 10,
                             t.address,
                             cold ? fmt::format("unchecked for {} frames", frame_ - t.frame)
                                  : std::string("still in use"));
      }
    }
    if (live_per_content_.find(content) == live_per_content_.end()) {
      ++distinct_contents_;
    }
    live_per_content_.emplace(content, key);
    texture.content_key = content;
  }

  // Every 10 s, if there were new textures with a hash. The cache snapshot (recorded, distinct contents)
  // is O(1).
  void ReportReuse() {
    if (diag_reuse_ <= 0) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - report_reuse_ < std::chrono::seconds(10)) {
      return;
    }
    report_reuse_ = now;
    if (reuse_new_) {
      const uint64_t pointed = uint64_t(live_per_content_.size());
      MASSEFFECT_REPORT_RING(
          "[native] C3 reuse by content (measurement only), last 10 s: {} new textures with fingerprint "
          "({:.1f} MB); {} with the same content and shape as another live one ({:.1f} MB), {} of them with one "
          "unchecked for more than {} frames ({:.1f} MB); {} with more than {} equal ones not fully inspected | in the "
          "cache: {} textures with fingerprint in {} distinct contents ({} repeated)",
          reuse_new_, double(reuse_bytes_new_) / 1048576.0, reuse_match_,
          double(reuse_bytes_match_) / 1048576.0, reuse_cold_, kFramesNoUsageForDrop,
          double(reuse_bytes_cold_) / 1048576.0, reuse_no_look_, kReuseLookMax, pointed,
          distinct_contents_, pointed - std::min(pointed, distinct_contents_));
    }
    reuse_new_ = 0;
    reuse_match_ = 0;
    reuse_cold_ = 0;
    reuse_no_look_ = 0;
    reuse_bytes_new_ = 0;
    reuse_bytes_match_ = 0;
    reuse_bytes_cold_ = 0;
  }

  // Every 30 s, one line with what hash sampling has done (see PrepareTexture). The ring thread writes
  // it, but it is a single short line every 30 s.
  void ReportSampling(std::chrono::steady_clock::time_point now) {
    if (now - report_sampling_ < std::chrono::seconds(30)) {
      return;
    }
    if (report_sampling_ != std::chrono::steady_clock::time_point{}) {
      MASSEFFECT_REPORT_RING("[native] C3 fingerprint sampling: {} rechecks resolved with the sample ({:.1f} MB read and "
                  "{:.1f} MB unread), {} with a changed sample and {} with both fingerprints since the previous line; "
                  "{} with both since the start",
                  samples_hits_, double(bytes_sample_) / 1048576.0,
                  double(bytes_saved_sample_) / 1048576.0, distinct_samples_, samples_with_the_two_,
                  checked_samples_);
    }
    report_sampling_ = now;
    samples_hits_ = 0;
    distinct_samples_ = 0;
    samples_with_the_two_ = 0;
    bytes_sample_ = 0;
    bytes_saved_sample_ = 0;
  }

  // The blocks of one level (one layer) of a 2D texture or cubemap, from block (ox, oy) of its guest
  // region (the packed tail or the base of a small texture do not start at 0), to the host destination
  // bx_host blocks wide. Linear: row_bytes per row; tiled: pitch in blocks (GetTiledOffset2D). false if
  // it goes past memory.
  bool ReadLevel(uint64_t address, bool tile, uint32_t pitch_blocks, uint64_t row_bytes,
                 const TextureFormat& tf, uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                 uint8_t* target) {
    if (!bx || !by) {
      return true;
    }
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2 : tf.bytes >= 2 ? 1 : 0;
    const uint64_t end =
        tile ? uint64_t(std::max<int64_t>(TileDisplacement2D(int32_t((ox + bx + 31) & ~31u),
                                                                     int32_t((oy + by + 31) & ~31u),
                                                                     pitch_blocks, log2),
                                             0))
                : row_bytes * (oy + by - 1) + uint64_t(ox + bx) * tf.bytes;
    if (address + end > kPhysicalMemory) {
      return false;
    }
    const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
    // Opt-in measurement only: gather every USED raw block before the real
    // untiler, then compare staged bytes and the same guest blocks afterwards.
    // Never hash padding or read outside physical memory for the audit.
    const uint64_t audit_bytes = uint64_t(bx) * by * tf.bytes;
    bool audit_level = StartPayloadAudit(0, audit_bytes);
    std::vector<uint64_t> audit_offsets;
    std::vector<uint8_t> audit_raw;
    uint64_t audit_before = 0;
    if (audit_level) {
      audit_offsets.reserve(size_t(bx) * by);
      for (uint32_t y = 0; y < by && audit_level; ++y) {
        for (uint32_t x = 0; x < bx; ++x) {
          const int64_t offset = tile
              ? TileDisplacement2D(int32_t(ox + x), int32_t(oy + y), pitch_blocks, log2)
              : int64_t(row_bytes * (oy + y) + uint64_t(ox + x) * tf.bytes);
          if (offset < 0 || uint64_t(offset) > kPhysicalMemory ||
              address + uint64_t(offset) + tf.bytes > kPhysicalMemory) {
            audit_level = false;
            ++me_payload_invalid_;
            --me_payload_checked_[0];
            me_payload_bytes_[0] -= audit_bytes;
            break;
          }
          audit_offsets.push_back(uint64_t(offset));
        }
      }
      if (audit_level) {
        audit_raw.resize(size_t(audit_bytes));
        for (size_t i = 0; i < audit_offsets.size(); ++i)
          std::memcpy(audit_raw.data() + i * tf.bytes, source + audit_offsets[i], tf.bytes);
        audit_before = XXH3_64bits(audit_raw.data(), audit_raw.size());
      }
    }
    const auto finish_payload_level = [&] {
      if (!audit_level) return;
      for (uint32_t y = 0; y < by; ++y)
        std::memcpy(audit_raw.data() + size_t(y) * bx * tf.bytes,
                    target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes);
      const uint64_t staged = XXH3_64bits(audit_raw.data(), audit_raw.size());
      for (size_t i = 0; i < audit_offsets.size(); ++i)
        std::memcpy(audit_raw.data() + i * tf.bytes, source + audit_offsets[i], tf.bytes);
      FinalizePayloadAudit(0, address, audit_bytes, audit_before,
                            XXH3_64bits(audit_raw.data(), audit_raw.size()), staged);
    };
    if (!tile) {
      for (uint32_t y = 0; y < by; ++y) {
        std::memcpy(target + size_t(y) * bx_host * tf.bytes,
                    source + uint64_t(oy + y) * row_bytes + uint64_t(ox) * tf.bytes, size_t(bx) * tf.bytes);
      }
      finish_payload_level();
      return true;
    }
    // The fast untiling (UntileLevel) when the block is 1, 2, 4, 8 or 16 bytes.
    if (tile_fast_ < 0) {
      tile_fast_ = REXCVAR_GET(masseffect_native_tile_fast) ? 1 : 0;
      REXLOG_INFO("[native] C3: fast untiling (masseffect_native_tile_fast) = {}",
                  tile_fast_ ? "YES, checking the first levels against the regular path" : "no");
    }
    if (tile_fast_ && tf.bytes == (1u << log2)) {
      switch (log2) {
        case 0: UntileLevel<0>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 1: UntileLevel<1>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 2: UntileLevel<2>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        case 3: UntileLevel<3>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
        default: UntileLevel<4>(source, pitch_blocks, ox, oy, bx, by, bx_host, target); break;
      }
      /*
       * Guard: the first kLevelsToCheck levels are repeated on the usual path and compared byte by byte.
       * If one differs, the usual result is kept, a warning is logged and the fast path is off for the rest
       * of the session.
       */
      // The first 200, then 1 in 64 (checking every level, the guard cost 4.2 ms per MB).
      ++seen_levels_;
      if (seen_levels_ <= kLevelsToCheck || seen_levels_ % kCheckAOfEach == 0) {
        ++checked_levels_;
        check_tile_.assign(size_t(by) * bx_host * tf.bytes, 0);
        for (uint32_t y = 0; y < by; ++y) {
          std::memcpy(check_tile_.data() + size_t(y) * bx_host * tf.bytes,
                      target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes);
        }
        ReadLevelTileOfAlways(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
        bool equal = true;
        for (uint32_t y = 0; y < by && equal; ++y) {
          equal = std::memcmp(check_tile_.data() + size_t(y) * bx_host * tf.bytes,
                                target + size_t(y) * bx_host * tf.bytes, size_t(bx) * tf.bytes) == 0;
        }
        if (!equal) {
          REXLOG_ERROR("[native] C3: fast untiling does NOT match ({}x{} blocks of {} bytes, pitch {}, "
                       "from {},{}). It is SWITCHED OFF and the regular path is used.",
                       bx, by, tf.bytes, pitch_blocks, ox, oy);
          tile_fast_ = 0;
        } else if (checked_levels_ == kLevelsToCheck) {
          REXLOG_INFO("[native] C3: fast untiling: {} levels checked against the regular path, all "
                      "equal. The fast path continues.",
                      checked_levels_);
        }
      }
      finish_payload_level();
      return true;
    }
    ReadLevelTileOfAlways(source, pitch_blocks, log2, tf.bytes, ox, oy, bx, by, bx_host, target);
    finish_payload_level();
    return true;
  }

  // The original byte swap, word by word.
  static void OrderOfAlways(std::vector<uint8_t>& data, uint32_t unit, xenos::Endian order) {
    if (unit == 2 && order != xenos::Endian::kNone) {
      for (size_t i = 0; i + 1 < data.size(); i += 2) {
        uint16_t v;
        std::memcpy(&v, data.data() + i, 2);
        v = xenos::GpuSwap(v, order);
        std::memcpy(data.data() + i, &v, 2);
      }
    } else if (unit == 4 && order != xenos::Endian::kNone) {
      for (size_t i = 0; i + 3 < data.size(); i += 4) {
        uint32_t v;
        std::memcpy(&v, data.data() + i, 4);
        v = xenos::GpuSwap(v, order);
        std::memcpy(data.data() + i, &v, 4);
      }
    }
  }

  // The original untiling, block by block.
  static void ReadLevelTileOfAlways(const uint8_t* source, uint32_t pitch_blocks, uint32_t log2, uint32_t bytes,
                                        uint32_t ox, uint32_t oy, uint32_t bx, uint32_t by, uint32_t bx_host,
                                        uint8_t* target) {
    for (uint32_t y = 0; y < by; ++y) {
      for (uint32_t x = 0; x < bx; ++x) {
        const int32_t displacement =
            TileDisplacement2D(int32_t(ox + x), int32_t(oy + y), pitch_blocks, log2);
        std::memcpy(target + (size_t(y) * bx_host + x) * bytes, source + displacement, bytes);
      }
    }
  }

  // Base level of a 3D texture, slice by slice (z, then y, then x), in the order vkCmdCopyBufferToImage
  // uploads it. Tiled: GetTiledOffset3D; linear: consecutive slices with rows and columns aligned to 32
  // blocks (GetGuestTextureLayout).
  bool ReadVolume(const uint32_t* f, const TextureFormat& tf, uint32_t blocks_x,
                   uint32_t blocks_y, uint32_t background, uint32_t blocks_x_host,
                   uint32_t blocks_y_host, uint32_t pitch_blocks, bool tile,
                   std::vector<uint8_t>& data) {
    const uint64_t address = uint64_t((f[1] >> 12) << 12) & 0x1FFFFFFF;
    const uint32_t log2 = tf.bytes >= 16 ? 4 : tf.bytes >= 8 ? 3 : tf.bytes >= 4 ? 2
                                                             : tf.bytes >= 2 ? 1 : 0;
    const uint64_t row = uint64_t((pitch_blocks + 31) & ~uint32_t(31)) * tf.bytes;
    const uint64_t cut = row * ((blocks_y + 31) & ~uint32_t(31));
    const uint64_t end =
        tile ? uint64_t(std::max<int64_t>(
                      TileDisplacement3D(int32_t((blocks_x + 31) & ~31u),
                                              int32_t((blocks_y + 31) & ~31u),
                                              int32_t((background + 3) & ~3u), pitch_blocks,
                                              blocks_y, log2),
                      0))
                : cut * background;
    if (address + end > kPhysicalMemory) {
      return false;
    }
    const uint8_t* source = memory_->TranslatePhysical(uint32_t(address));
    const size_t bytes_cut = size_t(blocks_x_host) * blocks_y_host * tf.bytes;
    for (uint32_t z = 0; z < background; ++z) {
      for (uint32_t y = 0; y < blocks_y; ++y) {
        for (uint32_t x = 0; x < blocks_x; ++x) {
          const uint64_t displacement =
              tile ? uint64_t(TileDisplacement3D(int32_t(x), int32_t(y), int32_t(z),
                                                         pitch_blocks, blocks_y, log2))
                      : z * cut + y * row + uint64_t(x) * tf.bytes;
          std::memcpy(data.data() + bytes_cut * z + (size_t(y) * blocks_x_host + x) * tf.bytes,
                      source + displacement, tf.bytes);
        }
      }
    }
    return true;
  }

  // A texture's VkImageCreateInfo, factored out so CreateTexture and CreateTextureInThread create exactly the
  // same image.
  static VkImageCreateInfo InfoImageTexture(VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                                             uint32_t background, uint32_t levels) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = background ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.flags = layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.format = format;
    info.extent = {width, height, background ? background : 1};
    info.mipLevels = std::max(levels, 1u);
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
  }

  // `emergency` = false only from CollectBindings. Half the cache cannot be released there: that waits
  // for the GPU by submitting the work, and we are inside BeforeSend. Out of memory: false, and the
  // texture is recreated later.
  bool CreateTexture(ImageNative& image, VkFormat format, uint32_t width, uint32_t height,
                    uint32_t layers = 1, uint32_t background = 0, uint32_t levels = 1, bool emergency = true) {
    const VkImageCreateInfo info = InfoImageTexture(format, width, height, layers, background, levels);
    /*
     * The pool first, and the usual path only if it does not fit.
     *
     * A dedicated allocation per texture costs 1.9 ms of CPU on Horizon, measured inside the ioctls:
     * nvMapCreate 422 us, plus two GPU address reservations (127 us each) and two mappings (626 us each).
     * There are two of each because the dedicated allocation makes its own address and mapping, and then
     * nvk_image_plane_bind makes others for the plane. And the only thing the dedicated allocation buys is
     * compression, which our textures never use (nvk_image.c:843-846 returns false with
     * SAMPLED|TRANSFER_DST). So we paid double and got nothing.
     *
     * Sub-allocating from large slabs removes the allocation's address and mapping: 1.9 -> 0.75 ms. For a
     * burst of 15 textures in one frame, from 28.5 to 11.3 ms. That is what shows when entering a new zone
     * of the map.
     *
     * It does not change a single pixel: same tiling, same format, same pte_kind. Binding at an offset
     * keeps the block-linear layout (nvk_image.c:1700-1710) and is the normal path of any sub-allocator in
     * NVK.
     */
    bool reserved = false;
    if (pool_textures_.Active()) {
      VkImage image_pool = VK_NULL_HANDLE;
      if (dfn_.vkCreateImage(device_, &info, nullptr, &image_pool) == VK_SUCCESS) {
        VkMemoryRequirements req{};
        dfn_.vkGetImageMemoryRequirements(device_, image_pool, &req);
        VkDeviceMemory block = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint32_t id_block = 0xFFFFFFFFu;
        if (pool_textures_.Reserve(req, block, offset, id_block) &&
            dfn_.vkBindImageMemory(device_, image_pool, block, offset) == VK_SUCCESS) {
          image.image = image_pool;
          image.memory = VK_NULL_HANDLE;  // from the pool: not freed on its own
          image.pool_block = id_block;
          reserved = true;
        } else {
          if (id_block != 0xFFFFFFFFu) {
            pool_textures_.Release(id_block);
          }
          dfn_.vkDestroyImage(device_, image_pool, nullptr);
          pool_textures_.NoteDedicated();
        }
      }
    }
    if (!reserved) {
      image.pool_block = 0xFFFFFFFFu;
      reserved = rex::ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
          image.memory);
    }
    if (!reserved) {
      // Out of GPU memory. Instead of giving up (which ends in a black screen), half the cache is released
      // with the GPU idle and the allocation is retried once.
      // The retry always takes the dedicated path, on purpose. Getting here means memory is short, and the
      // dedicated path is the one that can ask the system for more; the pool only hands out what it already
      // has. pool_block is left invalid so DestroyImage does not touch the pool.
      image.pool_block = 0xFFFFFFFFu;
      if (!emergency || !DropTexturesPerMissingOfMemory() ||
          !rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
              image.memory)) {
        REXLOG_ERROR("[native] C3: out of GPU memory for a {}x{} texture ({} levels, {} layers) and "
                     "{}; {} textures and {} MB remain",
                     width, height, std::max(levels, 1u), layers,
                     emergency ? "releasing the cache was not enough"
                                : "without releasing the cache (from CollectBindings; it is recreated at its next check)",
                     textures_.size(), bytes_textures_ >> 20);
        return false;
      }
    }
    image.width = width;
    image.height = height;
    image.format = format;
    image.prepared = false;
    return true;
  }

  bool UploadTexture(Texture& texture) {
    const VkCommandBuffer upload = context_->CommandsUpload();
    if (!upload || !texture.needs_upload) {
      return upload != VK_NULL_HANDLE;
    }
    VkDeviceSize offset;
    Reserve(texture.data.size(), 16, offset);  // BC: offset multiple of the block
    std::memcpy(upload_data_ + offset, texture.data.data(), texture.data.size());
    /*
     * If its vkBindImageMemory is still on the bind thread, neither the barrier nor the copy can be
     * recorded for the image (it has no memory). The data is already in the upload buffer; the barrier and
     * the copy are recorded in CollectBindings, in this same upload buffer before it is closed
     * (BeforeSend).
     */
    if (texture.in_flight && !PostponeCopy(texture, offset, upload)) {
      CollectBindings(true);  // inconsistent index (PostponeCopy already logged it and switched off): collect everything here
    }
    if (!texture.in_flight && texture.image.image != VK_NULL_HANDLE) {
      if (!texture.image.prepared) {
        Barrier(upload, texture.image.image, texture.layers);
        texture.image.prepared = true;
      }
      RecordCopyTexture(upload, texture, offset);
    }
    texture.needs_upload = false;
    /*
     * The copy of the pixels is no longer needed. They are already in the upload buffer, and to know
     * whether the texture changes its hashes are kept, not the bytes. Keeping the copy made the texture
     * cache take the same space again in CPU RAM (150-680 MB). That once ended in std::bad_alloc in the
     * main menu, with the cache at 682 MB.
     */
    // The larger buffer is kept as temporal_ (so the next new texture does not ask the system for memory
    // again or touch fresh pages); the other is released.
    if (texture.data.capacity() > temporal_.capacity()) {
      temporal_.swap(texture.data);
    }
    std::vector<uint8_t>().swap(texture.data);
    ++uploads_texture_;
    return true;
  }

  // One range per mip level (with all its layers). Factored out for UploadTexture and for the deferred
  // copies of CollectBindings.
  void RecordCopyTexture(VkCommandBuffer upload, const Texture& texture, VkDeviceSize offset) {
    // A guest texture may be uploaded more than once within one submission.
    // Order overlapping transfer writes as well as writes from earlier work.
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before,
                              0, nullptr, 0, nullptr);
    std::array<VkBufferImageCopy, 16> copies{};
    for (uint32_t n = 0; n < texture.levels; ++n) {
      VkBufferImageCopy& copy = copies[n];
      copy.bufferOffset = offset + texture.level_displacement[n];
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, n, 0, texture.layers};
      copy.imageExtent = {std::max(texture.image.width >> n, 1u), std::max(texture.image.height >> n, 1u),
                           texture.background ? texture.background : 1};
    }
    dfn_.vkCmdCopyBufferToImage(upload, upload_, texture.image.image, VK_IMAGE_LAYOUT_GENERAL,
                                texture.levels, copies.data());
  }

  // A texture's view, the same for SlotView and for the deferred views of CollectBindings.
  VkResult CreateViewTexture(VkImage image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap,
                             VkImageView& view) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = heap == 2   ? VK_IMAGE_VIEW_TYPE_CUBE
                    : heap == 1 ? VK_IMAGE_VIEW_TYPE_3D
                                  : VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.components = MappingComponents(swizzle, swizzle_host);
    info.subresourceRange = {IsDepth(format)
                                 ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                 : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                             0, VK_REMAINING_MIP_LEVELS, 0, heap == 2 ? 6u : 1u};  // with its mips
    return dfn_.vkCreateImageView(device_, &info, nullptr, &view);
  }

  // The views_ key (image, heap and both swizzles), factored out for CollectBindings.
  static uint64_t ViewKey(VkImage image, uint32_t swizzle, uint16_t swizzle_host, uint32_t heap) {
    return XXH3_64bits_withSeed(&image, sizeof(image),
                                (uint64_t(heap) << 40) | (uint64_t(swizzle) << 16) | swizzle_host);
  }

  // Heap slot (0 for 2D textures, 2 for cubemaps) for a view of the image with that swizzle.
  uint32_t SlotView(VkImage image, VkFormat format, uint32_t swizzle, uint16_t swizzle_host,
                       uint32_t heap = 0) {
    const uint64_t key = ViewKey(image, swizzle, swizzle_host, heap);  // The same key
    if (const auto it = views_.find(key); it != views_.end()) {
      return it->second.slot;
    }
    // An image whose vkBindImageMemory is still on the bind thread. The view carries the image's GPU
    // address, which comes from the bind: the slot is reserved now, and the view is created and written in
    // CollectBindings, before submission (UPDATE_AFTER_BIND descriptors).
    if (!images_in_flight_.empty() && images_in_flight_.count(image)) {
      return SlotViewInFlight(key, image, format, swizzle, swizzle_host, heap);
    }
    View view;
    view.image = image;
    view.heap = heap;
    if (CreateViewTexture(image, format, swizzle, swizzle_host, heap, view.view) != VK_SUCCESS) {
      Warn(34, "could not create a texture view");
      return 0;
    }
    view.slot = ReserveSlot(heap);
    if (!view.slot) {
      dfn_.vkDestroyImageView(device_, view.view, nullptr);
      Warn(35, "texture pool full");
      return 0;
    }
    WriteImage(heap, view.slot, view.view);
    views_.emplace(key, view);
    views_per_image_[image].push_back(key);
    return view.slot;
  }

  // point: no filtering (depth textures).
  uint32_t SlotSampler(const uint32_t* f, bool punctual = false) {
    // Diagnostic: filters the game requests that this renderer does not apply (anisotropy, mip bias and
    // maximum mip level; the bias is not even part of the key). Each new value is logged once.
    {
      const uint32_t aniso = (f[3] >> 25) & 0x7;
      const int32_t bias = int32_t(f[4] << 10) >> 22;  // lod_bias: 10 bits signed, 5 fractional
      const uint32_t mip_max = (f[4] >> 6) & 0xF;
      const uint32_t bias_index = uint32_t(bias + 512);
      if (!((aniso_seen_ >> aniso) & 1u) || !seen_biases_[bias_index] || !((mip_max_seen_ >> mip_max) & 1u)) {
        aniso_seen_ |= 1u << aniso;
        seen_biases_[bias_index] = true;
        mip_max_seen_ |= 1u << mip_max;
        REXLOG_INFO("[native] C4 filters requested by the game (new value): anisotropic {}, mip bias {:.3f}, "
                    "max mip level {}, mag/min/mip filters {}/{}/{}{}",
                    aniso, float(bias) / 32.0f, mip_max, (f[3] >> 19) & 0x3, (f[3] >> 21) & 0x3,
                    (f[3] >> 23) & 0x3, punctual ? " (point sampling)" : "");
      }
    }
    const uint32_t field_filters = (f[3] >> 19) & 0xFFF;  // mag in 0-1, min in 2-3, mip in 4-5
    const uint32_t field_mips = (f[4] >> 2) & 0xFF;       // minimum in 0-3, maximum in 4-7
    const uint32_t key = ((f[0] >> 10) & 0x1FF) | (field_filters << 9) |
                           (field_mips << 21) | ((f[5] & 0x3) << 29) |
                           (punctual ? 0x80000000u : 0u);
    if (const auto it = samplers_.find(key); it != samplers_.end()) {
      return it->second.second;
    }
    static constexpr VkSamplerAddressMode kModes[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE};
    const bool mirror = vulkan_device_->properties().samplerMirrorClampToEdge;
    const auto mode = [&](uint32_t value) {
      const VkSamplerAddressMode m = kModes[value & 0x7];
      return m == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !mirror
                 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                 : m;
    };
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter =
        ((f[3] >> 19) & 0x3) == 1 && !punctual ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter =
        ((f[3] >> 21) & 0x3) == 1 && !punctual ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const uint32_t mip = (field_filters >> 4) & 0x3;  
    info.mipmapMode =
        mip == 1 && !punctual ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = mode(f[0] >> 10);
    info.addressModeV = mode(f[0] >> 13);
    info.addressModeW = mode(f[0] >> 16);
    info.minLod = float(field_mips & 0xF);  // with the mip diagnostic if it is enabled
    // With real mips, the fetch constant's maximum level also limits (like the emulation's sampler);
    // single-level textures stay as before.
    info.maxLod = mip == 2 ? info.minLod + 0.25f : std::max(info.minLod, float((f[4] >> 6) & 0xF));
    info.borderColor = (f[5] & 0x3) == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                         : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    // masseffect_native_filter_aniso: the game's anisotropy (fetch bits 25-27, already in the key through the
    // filter field), as the SDK applies it. Only samplers with mips; point (depth) samplers stay as they are.
    {
      static const int32_t filter_aniso = REXCVAR_GET(masseffect_native_filter_aniso);
      const uint32_t aniso = (f[3] >> 25) & 0x7;
      if (filter_aniso > 0 && !punctual && aniso >= 1 && aniso <= 5 && info.maxLod > info.minLod) {
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        const auto& properties = vulkan_device_->properties();
        if (filter_aniso > 1 && aniso > 1 && properties.samplerAnisotropy) {
          info.anisotropyEnable = VK_TRUE;
          info.maxAnisotropy = std::min(float(1u << (aniso - 1)), properties.maxSamplerAnisotropy);
        }
      }
    }
    // masseffect_anisotropic_native. Only linear samplers with mips (maxLod above minLod); the value is read at
    // start-up, so it does not need to be in the key: every sampler is created with the same one.
    {
      static const uint32_t aniso_request = uint32_t(std::max(0, int32_t(REXCVAR_GET(masseffect_anisotropic_native))));
      const auto& properties = vulkan_device_->properties();
      if (aniso_request > 1 && properties.samplerAnisotropy && info.minFilter == VK_FILTER_LINEAR &&
          info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR && info.maxLod > info.minLod) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::max(1.0f, std::min(float(aniso_request), properties.maxSamplerAnisotropy));
        if (!aniso_recorded_) {
          aniso_recorded_ = true;
          REXLOG_INFO("[native] C4 anisotropic filtering {} on linear samplers with mips (requested {}, device "
                      "limit {}); single-level and point samplers unchanged",
                      info.maxAnisotropy, aniso_request, properties.maxSamplerAnisotropy);
        }
      }
    }
    VkSampler sampler;
    if (dfn_.vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS) {
      Warn(36, "could not create a sampler");
      return 0;
    }
    const uint32_t slot = ReserveSlot(3);
    if (!slot) {
      dfn_.vkDestroySampler(device_, sampler, nullptr);
      Warn(37, "sampler pool full");
      return 0;
    }
    WriteSampler(slot, sampler);
    samplers_.emplace(key, std::make_pair(sampler, slot));
    return slot;
  }

  bool PsAlphaOnly() { return REXCVAR_GET(masseffect_native_ps_alpha_only); }

  bool NoPsNoColor() { return REXCVAR_GET(masseffect_native_no_ps_no_color); }

  // 1/size of a slot's host image, in the shared constants. With size 0 (a texture that could not be
  // prepared) 0 is written: the shader does not use it because it does not sample either.
  static void WriteInvSize(uint32_t* shared, uint32_t register_value, uint32_t width, uint32_t height) {
    const float inv[2] = {width ? 1.0f / float(width) : 0.0f, height ? 1.0f / float(height) : 0.0f};
    std::memcpy(shared + kWordInvSize + register_value * 2, inv, sizeof(inv));
  }

  bool BeginPass(const uint32_t* r, const uint64_t keys[5], uint32_t pitch,
                   uint64_t pass_key) {
    pass_start_ = time_ ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    if (!PrepareEmpty()) {
      return false;
    }
    std::array<ImageNative*, 5> images{};
    for (uint32_t i = 0; i < 4; ++i) {
      if (keys[i]) {
        images[i] = context_->TargetColor(uint32_t(keys[i] >> 24) & 0xFFF,
                                              uint32_t(keys[i] >> 16) & 0xF, pitch);
        if (!images[i]) {
          return Reject(40, "no color target");
        }
      }
    }
    if (keys[4]) {
      images[4] = context_->TargetDepth(uint32_t(keys[4] >> 24) & 0xFFF,
                                                  uint32_t(keys[4] >> 16) & 0x1, pitch);
      if (!images[4]) {
        return Reject(41, "no depth target");
      }
    }
    (void)r;
    // The render targets are already resolved above; that segment is closed.
    const auto after_targets = time_ ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    if (time_) {
      stages_ns_[9] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          after_targets - pass_start_).count());
    }
    uint32_t width = UINT32_MAX, height = UINT32_MAX;
    uint32_t raster_grid_x = UINT32_MAX;
    std::array<VkImageView, 5> views{};
    uint32_t formats[5] = {};
    std::array<uint32_t, 5> sample_counts{};
    for (uint32_t i = 0; i < 5; ++i) {
      if (images[i]) {
        const uint32_t grid = images[i]->raster_grid_x;
        if (grid > 1 || (grid && (images[i]->width & ((1u << grid) - 1))))
          return Reject(41, "target with unsupported raster grid X");
        if (raster_grid_x != UINT32_MAX && raster_grid_x != grid)
          return Reject(41, "targets with incompatible raster grids X");
        raster_grid_x = grid;
        width = std::min(width, images[i]->width);
        height = std::min(height, images[i]->height);
        views[i] = images[i]->view;
        formats[i] = uint32_t(images[i]->format);
        sample_counts[i] = uint32_t(images[i]->sample_count);
      }
    }
    if (raster_grid_x == UINT32_MAX || !width || !height)
      return Reject(41, "pass without a valid raster extent");
    const uint32_t sample_count = me::native::CommonHardwareSampleCount(sample_counts);
    if (!sample_count)
      return Reject(41, "attachment hardware sample counts are invalid or incompatible");
    // Render-pass/pipeline and material shader variants are still 1x. Keep the
    // barrier explicit even if an externally supplied context returns 2x.
    if (!me::native::IsSingleSample(sample_count))
      return Reject(41, "real multisample draw pass is not enabled in this backend");
    const uint32_t logical_width = width >> raster_grid_x;
    // Shadow map: a depth-only target matching shadow dimensions (880x880, 1280x1280, 1600x1600).
    const bool is_shadows = pitch >= 1600 && formats[4] && !formats[0] && !formats[1] &&
                            !formats[2] && !formats[3];
    // GPU time per pass type (C2 report), by render target width.
    category_pass_ = CategoryOfTarget(pitch, keys);
    context_->MarkGpu(category_pass_);
    context_->DescribeMarkGpu((uint64_t(width) << 48) | (uint64_t(height) << 32) | (uint64_t(formats[0] & 0xFFFF) << 16) |
                                 (formats[4] & 0xFFFF));
    label_pass_ = true;
    const bool shadows_no_load =
        is_shadows && REXCVAR_GET(masseffect_native_pass_shadows_no_load);
    const VkRenderPass pass = PassFor(formats, shadows_no_load ? kLoadIgnore : kLoadRead);
    if (pass == VK_NULL_HANDLE) {
      return Reject(42, "could not create the render pass");
    }
    const VkFramebuffer framebuffer = FramebufferFor(pass, views, width, height);
    if (framebuffer == VK_NULL_HANDLE) {
      return Reject(43, "could not create the framebuffer");
    }
    if (time_) {
      stages_ns_[10] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - after_targets).count());
    }
    const auto before_open = time_ ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
    const VkCommandBuffer cmd = context_->CommandsWork();
    if (!cmd) {
      return false;
    }
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    // Only the rectangle the game uses (see masseffect_native_pass_area_util).
    uint32_t pass_height = height;
    if (area_util_) {
      const auto it = util_height_.find(pitch);
      if (it != util_height_.end() && it->second < height) {
        pass_height = it->second;
      }
    }
    start.renderArea.extent = {width, pass_height};
    // The statistics cover the whole pass, from here to after EndRenderPass.
    stats_pass_ = context_->BeginStats(category_pass_);
    const auto before_start = std::chrono::steady_clock::now();
    dfn_.vkCmdBeginRenderPass(cmd, &start, VK_SUBPASS_CONTENTS_INLINE);
    ns_render_pass_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_start).count());
    if (time_) {
      stages_ns_[11] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - before_open).count());
    }
    active_pass_ = true;
    ++started_passes_;
    texels_passes_ += uint64_t(width) * pass_height;  // how much tile is loaded by loadOp = LOAD
    {
      // Same criterion as the per-category GPU time: the render target's pitch decides.
      const uint32_t category = CategoryOfTarget(pitch, keys);
      texels_per_category_[category] += uint64_t(width) * pass_height;
      ++passes_per_category_[category];
    }
    pass_commands_ = cmd;
    draws_in_pass_ = 0;  // position within the pass (deferred sky report)
    pass_key_ = pass_key;
    std::memcpy(pass_keys_, keys, sizeof(pass_keys_));  // the bytes that produce pass_key_
    pass_valid_keys_ = true;
    ++pass_series_;  // the cached framing depends on the pass size and scale
    pass_generation_ = context_->GenerationCommands();
    pass_width_ = width;
    pass_height_ = height;
    // Grid expansion changes only physical X coverage, not the shadow scale
    // (which applies in both axes). Derive the latter from the logical width.
    pass_raster_scale_x_ = float(1u << raster_grid_x);
    pass_scale_ = (is_shadows && pitch && logical_width && logical_width != pitch)
                       ? float(logical_width) / float(pitch)
                       : 1.0f;
    std::copy(std::begin(formats), std::end(formats), std::begin(pass_formats_));
    pass_depth_float24_half_ = images[4] && images[4]->depth_float24_half;
    pass_rp_ = pass;
    // Phase 0b (masseffect_native_pipeline_between_passes). Vulkan keeps the bound pipeline across passes of the
    // same buffer: the pass's first draw only binds again if its key differs. A new buffer still starts
    // with nothing bound (Draw, recording_generation_).
    if (!pipeline_between_passes_) {
      pipeline_bound_ = VK_NULL_HANDLE;
    } else if (pipeline_bound_ != VK_NULL_HANDLE) {
      ++passes_with_pipeline_;
    }
    recorded_state_ = false;
    if (pipeline_bound_ == VK_NULL_HANDLE) {
      eds_valid_ = false;  // pass that forgets the pipeline (without 0b): set everything again
    }
    return true;
  }

  // ZCULL: three load modes. kLoadClear is needed to clear the depth of images created without
  // TRANSFER_DST, the only ones eligible for a ZCULL plane.
  enum : uint32_t { kLoadRead = 0, kLoadIgnore = 1, kLoadClear = 2 };

  VkRenderPass PassFor(const uint32_t formats[5], uint32_t load_mode = kLoadRead) {
    // The mode is part of the key: two render passes with the same formats but a different loadOp are
    // different.
    const uint64_t key =
        XXH3_64bits_withSeed(formats, sizeof(uint32_t) * 5, load_mode);
    if (const auto it = passes_.find(key); it != passes_.end()) {
      return it->second;
    }
    const VkRenderPass pass = CreatePass(formats, load_mode);  // the usual code, factored out
    if (pass == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
    passes_.emplace(key, pass);
    return pass;
  }

  // The plain loop for 16-bit indices (byte-swapped or not), untouched.
  static void IndicesFrom16Scale(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& min,
                                 uint32_t& max) {
    min = 0xFFFF;
    max = 0;
    if (rotate) {
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t raw;
        std::memcpy(&raw, data + size_t(i) * 2, 2);
        const uint16_t v = std::byteswap(raw);
        output[i] = v;
        min = std::min<uint32_t>(min, v);
        max = std::max<uint32_t>(max, v);
      }
    } else {
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t v;
        std::memcpy(&v, data + size_t(i) * 2, 2);
        output[i] = v;
        min = std::min<uint32_t>(min, v);
        max = std::max<uint32_t>(max, v);
      }
    }
  }

  // The same loop with NEON, 16 indices per iteration (vrev16 swaps the bytes of each 16-bit index, the
  // same as std::byteswap), and the remainder with the plain loop. NEON loads and stores need no
  // alignment on AArch64.
  static void IndicesFrom16Neon(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& min,
                              uint32_t& max) {
    uint32_t i = 0;
    uint32_t mn = 0xFFFF;
    uint32_t mx = 0;
    if (count >= 16) {
      uint16x8_t min_a = vdupq_n_u16(0xFFFF);
      uint16x8_t min_b = min_a;
      uint16x8_t max_a = vdupq_n_u16(0);
      uint16x8_t max_b = max_a;
      // Two loops (swapping is fixed per draw): no decision inside the iteration.
      if (rotate) {
        for (; i + 16 <= count; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(data + size_t(i) * 2)));
          const uint16x8_t b = vreinterpretq_u16_u8(vrev16q_u8(vld1q_u8(data + size_t(i) * 2 + 16)));
          vst1q_u16(output + i, a);
          vst1q_u16(output + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      } else {
        for (; i + 16 <= count; i += 16) {
          const uint16x8_t a = vreinterpretq_u16_u8(vld1q_u8(data + size_t(i) * 2));
          const uint16x8_t b = vreinterpretq_u16_u8(vld1q_u8(data + size_t(i) * 2 + 16));
          vst1q_u16(output + i, a);
          vst1q_u16(output + i + 8, b);
          min_a = vminq_u16(min_a, a);
          min_b = vminq_u16(min_b, b);
          max_a = vmaxq_u16(max_a, a);
          max_b = vmaxq_u16(max_b, b);
        }
      }
      mn = vminvq_u16(vminq_u16(min_a, min_b));
      mx = vmaxvq_u16(vmaxq_u16(max_a, max_b));
    }
    for (; i < count; ++i) {
      uint16_t v;
      std::memcpy(&v, data + size_t(i) * 2, 2);
      if (rotate) {
        v = std::byteswap(v);
      }
      output[i] = v;
      mn = std::min<uint32_t>(mn, v);
      mx = std::max<uint32_t>(mx, v);
    }
    min = mn;
    max = mx;
  }

  // The 16-bit indices of a draw (see masseffect_native_indices_neon). Ring only.
  void IndicesFrom16(const uint8_t* data, uint32_t count, uint16_t* output, bool rotate, uint32_t& min,
                   uint32_t& max) {
    if (indices_neon_ < 0) {
      indices_neon_ = REXCVAR_GET(masseffect_native_indices_neon) ? 1 : 0;
      REXLOG_INFO("[native] C6 16-bit indices: {}",
                  indices_neon_ ? "with NEON; the first 20,000 draws are checked, then 1 in 4,096"
                                : "with the plain loop (masseffect_native_indices_neon = false)");
    }
    if (indices_neon_ == 0) {
      IndicesFrom16Scale(data, count, output, rotate, min, max);
      return;
    }
    IndicesFrom16Neon(data, count, output, rotate, min, max);
    const uint64_t n = ++indices_neon_draws_;
    if (n > 20000 && (n & 4095) != 0) {
      return;
    }
    indices_neon_test_.resize(count);
    uint32_t mn = 0;
    uint32_t mx = 0;
    IndicesFrom16Scale(data, count, indices_neon_test_.data(), rotate, mn, mx);
    ++indices_neon_checked_;
    if (mn != min || mx != max ||
        (count && std::memcmp(indices_neon_test_.data(), output, size_t(count) * 2) != 0)) {
      // The plain path stays for this draw and for the rest of the session.
      if (count) {
        std::memcpy(output, indices_neon_test_.data(), size_t(count) * 2);
      }
      min = mn;
      max = mx;
      indices_neon_ = 0;
      REXLOG_ERROR("[native] C6 16-bit indices: MISMATCH between NEON and the plain loop ({} indices, {}; min "
                   "{} vs {}, max {} vs {}). Switched off for the rest of the session: plain loop",
                   count, rotate ? "rotated" : "not rotated", min, mn, max, mx);
      return;
    }
    if (indices_neon_checked_ == 20000) {
      REXLOG_INFO("[native] C6 16-bit indices: 20,000 draws checked against the plain loop, 0 "
                  "mismatches; still checking 1 in 4,096");
    }
  }

  // The PassFor render pass without its cache, with nothing changed. The pipeline prewarm also uses it
  // from its thread (a compatible one: the same formats). It only reads its arguments.
  VkRenderPass CreatePass(const uint32_t formats[5], uint32_t load_mode) const {
    std::array<VkAttachmentDescription, 5> attached{};
    std::array<VkAttachmentReference, 4> colors{};
    VkAttachmentReference depth{};
    uint32_t n = 0, n_colors = 0;
    for (uint32_t i = 0; i < 5; ++i) {
      if (!formats[i]) {
        continue;
      }
      VkAttachmentDescription& a = attached[n];
      a.format = VkFormat(formats[i]);
      a.samples = VK_SAMPLE_COUNT_1_BIT;
      // With the test active, the shadow map does not load its previous content: the game clears it before
      // drawing it, so fetching the 1600x1600 tile only to throw it away is wasted work.
      a.loadOp = load_mode == kLoadIgnore ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                 : load_mode == kLoadClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                              : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      a.stencilLoadOp = i != 4                       ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                        : load_mode == kLoadClear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                     : VK_ATTACHMENT_LOAD_OP_LOAD;
      a.stencilStoreOp = i == 4 ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
      a.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      if (i < 4) {
        colors[n_colors++] = {n, VK_IMAGE_LAYOUT_GENERAL};
      } else {
        depth = {n, VK_IMAGE_LAYOUT_GENERAL};
      }
      ++n;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = n_colors;
    subpass.pColorAttachments = colors.data();
    subpass.pDepthStencilAttachment = formats[4] ? &depth : nullptr;
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = n;
    info.pAttachments = attached.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    // GENERAL layouts do not establish memory dependencies. These attachments
    // are loaded again by later passes and also read by resolves / sampling.
    // The implicit external dependencies have no matching memory access scopes.
    const VkPipelineStageFlags attachment_stages =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags attachment_access =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[0].dstStageMask = attachment_stages;
    // Mass Effect on NVK: MEMORY_WRITE expands to storage writes, which makes every pass begin flush L1 and
    // the shader caches (with a wait-for-idle). Compute and copy writes into attachments already end with
    // their own barriers, so only earlier passes' attachment writes (and transfers) need ordering here.
    dependencies[0].srcAccessMask = REXCVAR_GET(masseffect_native_pass_narrow_dependency)
        ? VkAccessFlags(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_TRANSFER_WRITE_BIT)
        : VkAccessFlags(VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    dependencies[0].dstAccessMask = attachment_access;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = attachment_stages;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dependencies[1].srcAccessMask = attachment_access;
    dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    info.dependencyCount = uint32_t(dependencies.size());
    info.pDependencies = dependencies.data();
    VkRenderPass pass;
    if (dfn_.vkCreateRenderPass(device_, &info, nullptr, &pass) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pass;
  }

  VkFramebuffer FramebufferFor(VkRenderPass pass, const std::array<VkImageView, 5>& views,
                              uint32_t width, uint32_t height) {
    struct Key {
      VkRenderPass pass;
      std::array<VkImageView, 5> views;
      uint32_t width, height;
    } key{pass, views, width, height};
    const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
    if (const auto it = framebuffers_.find(fingerprint); it != framebuffers_.end()) {
      return it->second;
    }
    std::array<VkImageView, 5> attached{};
    uint32_t n = 0;
    for (VkImageView view : views) {
      if (view != VK_NULL_HANDLE) {
        attached[n++] = view;
      }
    }
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = n;
    info.pAttachments = attached.data();
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebuffer framebuffer;
    if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    framebuffers_.emplace(fingerprint, framebuffer);
    framebuffers_views_[fingerprint] = views;  // For ForgetView
    return framebuffer;
  }

  /*
   * The early Z and invisible draws report, every 10 s like the others.
   *
   * What matters is not what is saved but the second figure: "CANNOT: they write depth"
   * is, draw by draw, the exact size of what only a depth pre-pass could address, and that is expensive
   * to implement. A small figure means a pre-pass is not worth the risk.
   */
  void ReportZEarly() {
    ++frames_z_;
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report_z_).count() < 10) {
      return;
    }
    last_report_z_ = now;
    const uint64_t seen = frames_z_ - frames_z_previous_;
    const double frames = double(seen ? seen : 1);
    std::array<uint64_t, kZCounts> d{};
    uint64_t total = 0;
    for (uint32_t i = 0; i < kZCounts; ++i) {
      d[i] = counts_z_[i] - counts_z_previous_[i];
      counts_z_previous_[i] = counts_z_[i];
      total += d[i];
    }
    frames_z_previous_ = frames_z_;
    ReportSky(frames);  // goes before the !total cutoff, which has nothing to do with the sky
    if (!total) {
      return;
    }
    const uint64_t late = d[kZSet] + d[kZWritesZ] + d[kZStencil];
    MASSEFFECT_REPORT_RING(
        "[native] C6 early Z per frame: {:.0f} draws benefit, {:.0f} CANNOT because "
        "they write depth (that is the size of a depth pre-pass), {:.0f} because of stencil; {:.0f} "
        "already tested depth first. Of those that forced shading "
        "first, {:.0f} % are fixed{}",
        double(d[kZSet]) / frames, double(d[kZWritesZ]) / frames,
        double(d[kZStencil]) / frames,
        double(d[kZAlreadyEarly]) / frames,
        late ? 100.0 * double(d[kZSet]) / double(late) : 0.0,
        z_early_no_module_ ? fmt::format(" ({} shaders without a patched module)", z_early_no_module_)
                               : std::string());
    if (d[kInvisibleBlend] || d[kInvisibleAlpha] || d[kInvisibleColorOnly]) {
      MASSEFFECT_REPORT_RING(
          "[native] C6 invisible draws per frame: {:.1f} with a blend that copies the destination, {:.1f} "
          "with alpha test set to NEVER; {:.1f} are left with depth only",
          double(d[kInvisibleBlend]) / frames, double(d[kInvisibleAlpha]) / frames,
          double(d[kInvisibleColorOnly]) / frames);
    }
  }

  /*
   * The deferred sky report. Without it there is no way to measure whether it works.
   *
   * What to check: "deferred" must be ~0.9 per frame in play (the sky is a single draw and is
   * sometimes absent). "lost" must be 0: if not, some path closes the pass without going through
   * FinishPass and the sky is being lost. And "position" says how many draws of the pass were recorded
   * between the old place and the new one: exactly the amount of geometry that now covers the sky before
   * it is shaded. If it is small, so is the saving.
   */
  void ReportSky(double frames) {
    const uint64_t postponed = postponed_sky_count_ - postponed_previous_sky_;
    const uint64_t seen = seen_sky_ - seen_previous_sky_;
    if (!seen) {
      return;
    }
    std::array<uint64_t, kSkyReasons> per_reason{};
    uint64_t emitted = 0;
    for (uint32_t i = 0; i < kSkyReasons; ++i) {
      per_reason[i] = emitted_sky_[i] - emitted_previous_sky_[i];
      emitted_previous_sky_[i] = emitted_sky_[i];
      emitted += per_reason[i];
    }
    const uint64_t position_sum = sky_position_sum_ - sky_position_previous_sum_;
    const uint64_t candidates = sky_candidates_ - sky_previous_candidates_;
    sky_previous_candidates_ = sky_candidates_;
    seen_previous_sky_ = seen_sky_;
    postponed_previous_sky_ = postponed_sky_count_;
    sky_position_previous_sum_ = sky_position_sum_;
    /*
     * The line that decides whether the criterion works. "with the dome geometry" is the number that
     * matters: it must be 1.00 per frame. The "detected" ones are by fingerprint only and once gave 7.00,
     * which is what broke the image.
     */
    MASSEFFECT_REPORT_RING("[native] C6 sky: {:.2f} detected by fingerprint and {:.2f} with the dome geometry "
                "(480 indices and first in the pass) per frame. The second must be 1.00. "
                "GUARD: {} ({} frames with a dome examined, {} with exactly one, max {} in "
                "one frame)",
                double(seen) / frames, double(candidates) / frames,
                sky_guard_ == kSkyPostponing  ? "PASSED, deferring"
                : sky_guard_ == kDiscardedSky ? "DISCARDED, nothing is deferred"
                                                     : "still looking",
                sky_guard_frames_, sky_guard_with_one_, sky_guard_max_);
    MASSEFFECT_REPORT_RING(
        "[native] C6 deferred sky ({}): {:.2f} detected per frame, {:.2f} really deferred "
        "({} not deferrable in total, {} with another sky already pending); emitted {} when a draw "
        "with blending arrived, {} when an opaque draw that does not write Z arrived, {} at pass close, {} by another sky; "
        "LOST {}; average position {:.0f} draws ahead of it (max {})",
        postponed_sky_ ? "on" : "OFF", double(seen) / frames,
        double(postponed) / frames, sky_no_postponable_, sky_two_in_pass_,
        per_reason[kSkyPerBlend], per_reason[kSkyPerNoZ], per_reason[kSkyPerPassEnd],
        per_reason[kSkyPerOtherSky], lost_sky_,
        emitted ? double(position_sum) / double(emitted) : 0.0, sky_position_max_);
  }

  /*
   * Closes the frame for the sky guard. See the block of fields.
   *
   * Runs once per frame and does nothing once the guard has decided, so its cost is one comparison. The
   * decision is taken only once per session and logged.
   */
  void CloseFrameOfTheGuardOfTheSky() {
    const uint32_t in_this = sky_guard_in_frame_;
    sky_guard_in_frame_ = 0;
    if (sky_guard_ != kSkyLooking) {
      return;
    }
    /*
     * A frame without a sky says nothing about the criterion: menus, the logo and loading screens, which
     * come before gameplay. If they counted, the test would run out in the menu and the guard would switch
     * off for good without ever seeing the dome. Only frames in which the dome appears count.
     */
    if (in_this == 0) {
      return;
    }
    sky_guard_max_ = std::max(sky_guard_max_, in_this);
    if (in_this == 1) {
      ++sky_guard_with_one_;
    }
    if (++sky_guard_frames_ < kSkyFramesTest) {
      return;
    }
    /*
     * The test is over. It only switches on if it came out clean: never more than one, and enough frames
     * with exactly one. In any other case it stays off and the image is identical to the non-deferred one,
     * which is known to work.
     */
    if (sky_guard_max_ == 1 && sky_guard_with_one_ >= kSkyFramesWithOne) {
      sky_guard_ = kSkyPostponing;
      REXLOG_INFO("[native] C6 sky: guard PASSED ({} of {} frames with exactly one dome, "
                  "never two). The sky is deferred from now on: that is 3.4-4.3 ms of GPU without "
                  "changing a pixel",
                  sky_guard_with_one_, sky_guard_frames_);
    } else {
      sky_guard_ = kDiscardedSky;
      REXLOG_WARN("[native] C6 sky: guard NOT passed (max {} domes in one frame, {} of {} "
                  "frames with exactly one; {} were needed). NOTHING is deferred: the image "
                  "stays exactly as it was",
                  sky_guard_max_, sky_guard_with_one_, sky_guard_frames_,
                  kSkyFramesWithOne);
    }
  }

  VkShaderModule ModuleRectangle(const ShaderEntry& entry) {
    if (const auto it = modules_rectangle_.find(&entry); it != modules_rectangle_.end()) return it->second;
    const auto it = shaders_rectangle_.find(&entry);
    if (it == shaders_rectangle_.end() || it->second.words.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = it->second.words.size()*sizeof(uint32_t); info.pCode = it->second.words.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) module = VK_NULL_HANDLE;
    modules_rectangle_.emplace(&entry, module);
    return module;
  }

  // Track the CPU code of the ACTUAL final module, including a failed optional
  // variant's fallback. Normal PS modules borrow immutable library storage;
  // only locally transformed/diagnostic modules own a copy. Prewarm removes its
  // ephemeral handles before destroying them, so Vulkan handle reuse is safe.
  void RegistrarModuleCode(VkShaderModule module, const std::vector<uint32_t>& code,
                              bool borrow = false) {
    if (!module) return;
    std::lock_guard<std::mutex> lock(modules_depth_mutex_);
    ModuleCodeDepth source;
    if (borrow) source.borrowed = &code;
    else source.owned = code;
    codes_modules_depth_.insert_or_assign(module, std::move(source));
  }

  // Mass Effect: bit L set = the fragment module reads input Location L. Vertex shaders (package v30+) drop
  // every output the pixel shader does not read through specialization constant 60 (NVK links no
  // varyings: unread outputs and their math were computed for every vertex). Unknown code: all bits.
  uint32_t InputsMaskPS(VkShaderModule ps) {
    if (!ps || !REXCVAR_GET(masseffect_native_vs_pruned_outputs)) return 0xFFFFFFFFu;
    std::lock_guard<std::mutex> lock(modules_depth_mutex_);
    if (const auto it = masks_inputs_ps_.find(ps); it != masks_inputs_ps_.end()) return it->second;
    const auto source = codes_modules_depth_.find(ps);
    if (source == codes_modules_depth_.end()) return 0xFFFFFFFFu;
    const std::vector<uint32_t>& code = source->second.borrowed ? *source->second.borrowed : source->second.owned;
    uint32_t mask = 0;
    bool valid = code.size() > 5 && code[0] == 0x07230203;
    std::unordered_map<uint32_t, uint32_t> locations;  // id -> Location
    std::unordered_set<uint32_t> inputs, builtins;
    for (size_t i = 5; valid && i < code.size();) {
      const uint32_t words = code[i] >> 16, op = code[i] & 0xFFFF;
      if (!words || i + words > code.size()) { valid = false; break; }
      if (op == 71 /*OpDecorate*/ && words >= 4 && code[i + 2] == 30 /*Location*/) locations[code[i + 1]] = code[i + 3];
      if (op == 71 && words >= 3 && code[i + 2] == 11 /*BuiltIn*/) builtins.insert(code[i + 1]);
      if (op == 59 /*OpVariable*/ && words >= 4 && code[i + 3] == 1 /*Input*/) inputs.insert(code[i + 2]);
      i += words;
    }
    for (const uint32_t id : inputs) {
      if (builtins.count(id)) continue;
      const auto it = locations.find(id);
      if (it == locations.end() || it->second >= 32) { valid = false; break; }
      mask |= 1u << it->second;
    }
    if (!valid) mask = 0xFFFFFFFFu;
    masks_inputs_ps_.emplace(ps, mask);
    return mask;
  }
  std::unordered_map<VkShaderModule, uint32_t> masks_inputs_ps_;

  void ForgetModuleCode(VkShaderModule module) {
    std::lock_guard<std::mutex> lock(modules_depth_mutex_);
    codes_modules_depth_.erase(module);
    masks_inputs_ps_.erase(module);
  }

  // Invert the supported native XY raster modification on the ACTUAL final
  // material variant exactly once, BEFORE any depth transform injects raw host
  // FragCoord loads. This preserves the current continuous HLSL iPos ABI; it is
  // not the SDK's floor-based paramgen or complete Xenos MSAA conformance.
  VkShaderModule ModuleGuestFragCoordXY(VkShaderModule selected, const PipelineKey& key,
                                       const ShaderEntry* vs, const ShaderEntry* ps) {
    std::lock_guard<std::mutex> lock(modules_depth_mutex_);
    const auto source = codes_modules_depth_.find(selected);
    std::string reason;
    uint64_t code_hash = 0;
    size_t code_words = 0;
    const bool grid = bool(key.specialization & kSpecRasterGridX);
    const bool phase = bool(key.rasterization & me::native::kNativeMsaa2PhaseProbe);
    const bool phase2 = bool(key.rasterization & me::native::kNativeMsaa2PhaseMode2);
    const auto mode = grid ? me::native::GuestFragCoordXYMode::GridX2 :
                      phase2 ? me::native::GuestFragCoordXYMode::Phase2 :
                               me::native::GuestFragCoordXYMode::Phase1;
    if (grid == phase || (phase2 && !phase)) {
      reason = "unsupported combined or absent guest XY raster mode";
    } else if (source == codes_modules_depth_.end()) {
      reason = "final PS has no tracked CPU SPIR-V";
    } else {
      const auto& code = source->second.borrowed ? *source->second.borrowed : source->second.owned;
      code_words = code.size();
      code_hash = XXH3_64bits(code.data(), code.size() * sizeof(uint32_t));
      auto& bucket = modules_fragcoord_xy_[code_hash];
      for (const auto& entry : bucket) {
        if (entry.mode == mode && entry.original == code) return entry.module;
      }
      std::vector<uint32_t> mapped;
      VkShaderModule module = VK_NULL_HANDLE;
      if (me::native::TransformGuestFragCoordXY(code, mapped, reason, mode)) {
        if (mapped == code) return selected; // No FragCoord; do not own/destroy the base twice.
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = mapped.size() * sizeof(uint32_t);
        info.pCode = mapped.data();
        if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) {
          reason = "vkCreateShaderModule failure";
          module = VK_NULL_HANDLE;
        }
      }
      bucket.push_back({code, mode, module}); // Cache failure, never an unchecked base fallback.
      if (module) {
        ModuleCodeDepth tracked;
        tracked.owned = std::move(mapped);
        codes_modules_depth_.insert_or_assign(module, std::move(tracked));
        REXLOG_INFO("[native] final material FragCoord XY remapped mode={}: PS n{} container {:016X}, "
                    "source SPIR-V {:016X}/{} words", uint32_t(mode),
                    ps ? int(ps->number) : -1, ps ? ps->fingerprint : 0, code_hash, code_words);
        return module;
      }
    }
    const uint64_t n = ++raster_grid_ps_rejected_;
    if (n <= 16 || (n & 255) == 0)
      REXLOG_ERROR("[native] expanded raster grid rejected final PS (reject {}): {}; "
                   "VS n{} container {:016X}, PS n{} container {:016X}, final SPIR-V {:016X}/{} words, "
                   "gridX={} phase={} raster={:08X} specialization={:08X}",
                   n, reason, vs ? int(vs->number) : -1, vs ? vs->fingerprint : 0,
                   ps ? int(ps->number) : -1, ps ? ps->fingerprint : 0, code_hash, code_words,
                   bool(key.specialization & kSpecRasterGridX),
                   bool(key.rasterization & me::native::kNativeMsaa2PhaseProbe),
                   key.rasterization, key.specialization);
    return VK_NULL_HANDLE;
  }

  VkShaderModule ModuleDepthHalf(VkShaderModule selected) {
    if (!selected) return VK_NULL_HANDLE;
    std::lock_guard<std::mutex> lock(modules_depth_mutex_);
    const auto source = codes_modules_depth_.find(selected);
    if (source == codes_modules_depth_.end()) {
      REXLOG_ERROR("[native] depth half final PS has no tracked CPU code; rejecting, NOT using unhalved PS");
      return VK_NULL_HANDLE;
    }
    const auto& code = source->second.borrowed ? *source->second.borrowed : source->second.owned;
    const uint64_t hash = XXH3_64bits(code.data(), code.size() * sizeof(uint32_t));
    auto& bucket = modules_depth_half_[hash];
    for (const auto& entry : bucket) {
      if (entry.original == code) return entry.module; // Full collision guard.
    }
    std::vector<uint32_t> half;
    std::string reason;
    VkShaderModule module = VK_NULL_HANDLE;
    if (me::native::TransformDepthHalf(code, half, reason)) {
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = half.size() * sizeof(uint32_t);
      info.pCode = half.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) {
        module = VK_NULL_HANDLE;
        reason = "vkCreateShaderModule failure";
      }
    }
    if (!module)
      REXLOG_ERROR("[native] final PS depth half transform failed (hash {:016X}): {}; "
                   "rejecting, NOT using unhalved PS", hash, reason);
    else
      REXLOG_INFO("[native] final PS depth half composed: hash {:016X}, {} -> {} words", hash, code.size(), half.size());
    bucket.push_back({code, module}); // Cache failures as failures, never originals.
    return module;
  }

  VkShaderModule ModuleDepthQuantize(VkShaderModule selected, bool round_float24,
                                     bool synthetic_depth_only = false) {
    std::lock_guard<std::mutex> lock(modules_depth_mutex_);
    if (!selected && !synthetic_depth_only) {
      REXLOG_ERROR("[native] FLOAT24 selected PS creation failed; rejecting, NOT substituting a depth-only PS");
      return VK_NULL_HANDLE;
    }
    std::vector<uint32_t> synthetic;
    const std::vector<uint32_t>* code = nullptr;
    if (selected) {
      const auto source = codes_modules_depth_.find(selected);
      if (source == codes_modules_depth_.end()) {
        REXLOG_ERROR("[native] FLOAT24 final PS has no tracked code; rejecting experiment");
        return VK_NULL_HANDLE;
      }
      code = source->second.borrowed ? source->second.borrowed : &source->second.owned;
    } else {
      synthetic = me::native::MakeDepthOnlyFragmentForQuantization();
      code = &synthetic;
    }
    const uint64_t hash = XXH3_64bits(code->data(), code->size() * sizeof(uint32_t));
    auto& bucket = modules_depth_quantize_[hash];
    for (const auto& entry : bucket)
      if (entry.rounded == round_float24 && entry.original == *code) return entry.module;
    std::vector<uint32_t> half, quantized;
    std::string reason;
    VkShaderModule module = VK_NULL_HANDLE;
    // Compose from the tracked FINAL color/kill variant, exactly once. Half
    // remapping alone leaves raster Z unchanged; quantization is the last step.
    if (me::native::TransformDepthHalf(*code, half, reason) &&
        me::native::TransformDepthQuantizeIncoming(half, quantized, reason, round_float24)) {
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = quantized.size() * sizeof(uint32_t);
      info.pCode = quantized.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS) {
        module = VK_NULL_HANDLE;
        reason = "vkCreateShaderModule failure";
      }
    }
    if (!module)
      REXLOG_ERROR("[native] FLOAT24 final PS transform failed hash={:016X} round={}: {}; "
                   "rejecting, NOT using an unquantized fallback", hash, round_float24, reason);
    else if (bucket.empty())
      REXLOG_INFO("[native] FLOAT24 final PS quantized: hash={:016X} round={} synthetic={} {} -> {} words",
                  hash, round_float24, !selected, code->size(), quantized.size());
    bucket.push_back({*code, round_float24, module});
    return module;
  }

  VkShaderModule ModuleFor(const ShaderEntry& entry) {
    if (const auto it = modules_.find(&entry); it != modules_.end()) {
      return it->second;
    }
    const auto& spirv = entry.shader->Spirv();
    const std::vector<uint32_t>& code = spirv;
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &shader_module) != VK_SUCCESS) {
      shader_module = VK_NULL_HANDLE;
    }
    modules_.emplace(&entry, shader_module);
    if (!entry.vertices) RegistrarModuleCode(shader_module, code, true);
    return shader_module;
  }

  // Pixel shader module without the color writes, one per shader. If it cannot be pruned, the normal one
  // is returned: the image is the same, only the saving is lost.
  VkShaderModule ModuleAlphaOnly(const ShaderEntry& entry) {
    if (const auto it = modules_alpha_only_.find(&entry); it != modules_alpha_only_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleFor(entry);
    }
    uint32_t removed = 0;
    const std::vector<uint32_t> pruned = PruneWritesOfColor(entry.shader->Spirv(), removed);
    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (!pruned.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = pruned.size() * sizeof(uint32_t);
      info.pCode = pruned.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &shader_module) != VK_SUCCESS) {
        shader_module = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[native] C5a: PS n{} without color writes: {} removed, {} words of {} ({})",
                entry.number, removed, pruned.size(), entry.shader->Spirv().size(),
                shader_module != VK_NULL_HANDLE ? "module created" : "the regular one is used");
    modules_alpha_only_.emplace(&entry, shader_module);
    RegistrarModuleCode(shader_module, pruned);
    return shader_module != VK_NULL_HANDLE ? shader_module : ModuleFor(entry);
  }

  VkShaderModule Module7e3(const ShaderEntry& entry, uint32_t outputs_mask) {
    const uint64_t key = (uint64_t(entry.number) << 32) | outputs_mask;
    if (const auto it = modules_7e3_.find(key); it != modules_7e3_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleFor(entry);
    }
    const std::vector<uint32_t> limited = LimitOutputs7e3(entry.shader->Spirv(), outputs_mask);
    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (!limited.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = limited.size() * sizeof(uint32_t);
      info.pCode = limited.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &shader_module) != VK_SUCCESS) {
        shader_module = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[native] C6: PS n{} 7e3 output mask {:X}: {}", entry.number,
                outputs_mask, shader_module != VK_NULL_HANDLE ? "limited" : "the regular module is used");
    modules_7e3_.emplace(key, shader_module);
    RegistrarModuleCode(shader_module, limited);
    return shader_module != VK_NULL_HANDLE ? shader_module : ModuleFor(entry);
  }

  /*
   * Pixel shader module with EarlyFragmentTests declared, one per shader. If that is not possible (it
   * already had it, it writes gl_FragDepth...), the normal one is returned: the image is the same and only
   * the saving is lost. It is logged once per shader so the log shows which ones qualified.
   */
  VkShaderModule ModuleZEarly(const ShaderEntry& entry) {
    if (const auto it = modules_z_early_.find(&entry); it != modules_z_early_.end()) {
      return it->second != VK_NULL_HANDLE ? it->second : ModuleFor(entry);
    }
    const char* reason = "";
    const std::vector<uint32_t> patched = WithEarlyTests(entry.shader->Spirv(), reason);
    VkShaderModule shader_module = VK_NULL_HANDLE;
    if (!patched.empty()) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = patched.size() * sizeof(uint32_t);
      info.pCode = patched.data();
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &shader_module) != VK_SUCCESS) {
        shader_module = VK_NULL_HANDLE;
      }
    }
    REXLOG_INFO("[native] C6 early Z: PS n{} {} ({} kills, {} words)", entry.number,
                shader_module != VK_NULL_HANDLE
                    ? std::string("tests depth before shading")
                    : fmt::format("stays as it was: {}", reason),
                entry.kills, entry.shader->Spirv().size());
    modules_z_early_.emplace(&entry, shader_module);
    RegistrarModuleCode(shader_module, patched);
    if (shader_module == VK_NULL_HANDLE) {
      ++z_early_no_module_;
    }
    return shader_module != VK_NULL_HANDLE ? shader_module : ModuleFor(entry);
  }

  /*
   * The fixed state of a pipeline, exactly as PipelineFor sets it. It is its usual code, moved here without
   * changing any computation: only its eight declarations become references into FixedPipelineState. Used
   * by PipelineFor and by the guards of the canonical key (phase 0a) and of dynamic state (phases 1 and 2),
   * which thus compare against what the pipeline really carries and not against another copy of the same
   * computations. blend.pAttachments points to fixed.blends: the structure is not copied.
   */
  struct FixedPipelineState {
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    VkPipelineViewportStateCreateInfo view{};
    VkPipelineRasterizationStateCreateInfo rasterization{};
    VkPipelineMultisampleStateCreateInfo sampling{};
    VkPipelineDepthStencilStateCreateInfo depth{};
    std::array<VkPipelineColorBlendAttachmentState, 4> blends{};
    uint32_t n_colors = 0;
    VkPipelineColorBlendStateCreateInfo blend{};
    FixedPipelineState() = default;
    FixedPipelineState(const FixedPipelineState&) = delete;
    FixedPipelineState& operator=(const FixedPipelineState&) = delete;
  };

  // warn = false from the prewarm thread (warnings belong to the ring only).
  void FillFixedState(const PipelineKey& key, FixedPipelineState& fixed, bool warn = true) {
    VkPipelineInputAssemblyStateCreateInfo& assembly = fixed.assembly;
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VkPrimitiveTopology(key.topology);
    assembly.primitiveRestartEnable = (key.rasterization & 0x8) ? VK_TRUE : VK_FALSE;

    VkPipelineViewportStateCreateInfo& view = fixed.view;
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo& rasterization = fixed.rasterization;
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = ((key.rasterization & 0x1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                           ((key.rasterization & 0x2) ? VK_CULL_MODE_BACK_BIT : 0);
    // PA_SU_SC_MODE_CNTL.face: 1 = the front face is clockwise (not confirmed on screen).
    rasterization.frontFace =
        (key.rasterization & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    rasterization.depthBiasEnable = (key.rasterization & 0x10) ? VK_TRUE : VK_FALSE;
    rasterization.depthClampEnable = (key.rasterization & me::native::kNativeDepthClamp) ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo& sampling = fixed.sampling;
    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    const uint32_t d = key.depth;
    VkPipelineDepthStencilStateCreateInfo& depth = fixed.depth;
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (key.formats[4]) {
      depth.depthTestEnable = (d >> 1) & 0x1;
      depth.depthWriteEnable = ((d >> 1) & 0x1) && ((d >> 2) & 0x1);
      depth.depthCompareOp = VkCompareOp((d >> 4) & 0x7);
      depth.stencilTestEnable = d & 0x1;
      depth.front.failOp = VkStencilOp((d >> 11) & 0x7);
      depth.front.passOp = VkStencilOp((d >> 14) & 0x7);
      depth.front.depthFailOp = VkStencilOp((d >> 17) & 0x7);
      depth.front.compareOp = VkCompareOp((d >> 8) & 0x7);
      if ((d >> 7) & 0x1) {
        depth.back.compareOp = VkCompareOp((d >> 20) & 0x7);
        depth.back.failOp = VkStencilOp((d >> 23) & 0x7);
        depth.back.passOp = VkStencilOp((d >> 26) & 0x7);
        depth.back.depthFailOp = VkStencilOp((d >> 29) & 0x7);
      } else {
        depth.back = depth.front;
      }
    }

    static constexpr VkBlendFactor kFactors[32] = {
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
    };
    const auto operation = [&](uint32_t op) {
      switch (op) {
        case 1:
          return VK_BLEND_OP_SUBTRACT;
        case 2:
          return VK_BLEND_OP_MIN;
        case 3:
          return VK_BLEND_OP_MAX;
        case 4:
          if (warn) {
            Warn(52, "blend with reverse subtract: subtract is used");
          }
          return VK_BLEND_OP_SUBTRACT;
        default:
          return VK_BLEND_OP_ADD;
      }
    };
    std::array<VkPipelineColorBlendAttachmentState, 4>& blends = fixed.blends;
    uint32_t& n_colors = fixed.n_colors;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!key.formats[i]) {
        continue;
      }
      const uint32_t m = key.blend[i];
      VkPipelineColorBlendAttachmentState& s = blends[n_colors++];
      s.colorWriteMask = (key.masks >> (i * 4)) & 0xF;
      const uint32_t src_data = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
      const uint32_t source_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7,
                     target_a = (m >> 24) & 0x1F;
      /*
       * Blending is only enabled if it is needed on the channels that are written.
       *
       * It used to require all six fields to be "1 x source + 0 x destination, ADD". But if the mask does
       * not write RGB, the three color factors do not matter, and the same goes for alpha. With blendEnable
       * false the ROP does not have to read the destination or go through the blend unit. The result is the
       * same.
       */
      const bool writes_rgb = (s.colorWriteMask & 0x7) != 0;
      const bool writes_alpha = (s.colorWriteMask & 0x8) != 0;
      const bool color_direct = src_data == 1 && target == 0 && op == 0;
      const bool direct_alpha = source_a == 1 && target_a == 0 && op_a == 0;
      s.blendEnable = (writes_rgb && !color_direct) || (writes_alpha && !direct_alpha);
      s.srcColorBlendFactor = kFactors[src_data];
      s.dstColorBlendFactor = kFactors[target];
      s.colorBlendOp = operation(op);
      s.srcAlphaBlendFactor = kFactors[source_a];
      s.dstAlphaBlendFactor = kFactors[target_a];
      s.alphaBlendOp = operation(op_a);
    }
    VkPipelineColorBlendStateCreateInfo& blend = fixed.blend;
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = n_colors;
    blend.pAttachments = blends.data();
  }

  // One stencil face, in what Vulkan looks at in the pipeline (masks and reference are dynamic).
  static bool SameFaceStencil(const VkStencilOpState& a, const VkStencilOpState& b) {
    return a.failOp == b.failOp && a.passOp == b.passOp && a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp;
  }

  /*
   * Two fixed states, field by field, in what Vulkan looks at: the blend equation only with blendEnable,
   * each half only if the mask writes its channels, the Z function only with a Z test, stencil operations
   * only with stencil. Returns the first differing field, or nullptr if there is none.
   */
  static const char* EffectiveDifference(const FixedPipelineState& a, const FixedPipelineState& b) {
    if (a.assembly.topology != b.assembly.topology) {
      return "topology";
    }
    if (a.assembly.primitiveRestartEnable != b.assembly.primitiveRestartEnable) {
      return "primitiveRestartEnable";
    }
    if (a.view.viewportCount != b.view.viewportCount || a.view.scissorCount != b.view.scissorCount) {
      return "viewportCount or scissorCount";
    }
    const VkPipelineRasterizationStateCreateInfo& ra = a.rasterization;
    const VkPipelineRasterizationStateCreateInfo& rb = b.rasterization;
    if (ra.polygonMode != rb.polygonMode || ra.lineWidth != rb.lineWidth ||
        ra.rasterizerDiscardEnable != rb.rasterizerDiscardEnable || ra.depthClampEnable != rb.depthClampEnable) {
      return "polygonMode, lineWidth, rasterizerDiscardEnable or depthClampEnable";
    }
    if (ra.cullMode != rb.cullMode) {
      return "cullMode";
    }
    if (ra.frontFace != rb.frontFace) {
      return "frontFace";
    }
    if (ra.depthBiasEnable != rb.depthBiasEnable) {
      return "depthBiasEnable";
    }
    if (a.sampling.rasterizationSamples != b.sampling.rasterizationSamples) {
      return "rasterizationSamples";
    }
    const VkPipelineDepthStencilStateCreateInfo& pa = a.depth;
    const VkPipelineDepthStencilStateCreateInfo& pb = b.depth;
    if (pa.depthTestEnable != pb.depthTestEnable) {
      return "depthTestEnable";
    }
    if (pa.depthWriteEnable != pb.depthWriteEnable) {
      return "depthWriteEnable";
    }
    if (pa.depthTestEnable && pa.depthCompareOp != pb.depthCompareOp) {
      return "depthCompareOp";
    }
    if (pa.depthBoundsTestEnable != pb.depthBoundsTestEnable) {
      return "depthBoundsTestEnable";
    }
    if (pa.stencilTestEnable != pb.stencilTestEnable) {
      return "stencilTestEnable";
    }
    if (pa.stencilTestEnable && !SameFaceStencil(pa.front, pb.front)) {
      return "front (stencil)";
    }
    if (pa.stencilTestEnable && !SameFaceStencil(pa.back, pb.back)) {
      return "back (stencil)";
    }
    if (a.n_colors != b.n_colors || a.blend.attachmentCount != b.blend.attachmentCount ||
        a.blend.logicOpEnable != b.blend.logicOpEnable) {
      return "attachmentCount or logicOpEnable";
    }
    for (uint32_t i = 0; i < a.n_colors && i < 4; ++i) {
      const VkPipelineColorBlendAttachmentState& x = a.blends[i];
      const VkPipelineColorBlendAttachmentState& y = b.blends[i];
      if (x.colorWriteMask != y.colorWriteMask) {
        return "colorWriteMask";
      }
      if (x.blendEnable != y.blendEnable) {
        return "blendEnable";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x7) &&
          (x.srcColorBlendFactor != y.srcColorBlendFactor || x.dstColorBlendFactor != y.dstColorBlendFactor ||
           x.colorBlendOp != y.colorBlendOp)) {
        return "color equation";
      }
      if (x.blendEnable && (x.colorWriteMask & 0x8) &&
          (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor || x.dstAlphaBlendFactor != y.dstAlphaBlendFactor ||
           x.alphaBlendOp != y.alphaBlendOp)) {
        return "alpha equation";
      }
    }
    return nullptr;
  }

  /*
   * Phase 0a (masseffect_native_canonical_key). A draw's key in canonical form. Same criterion as
   * CanonicalState (the counter) with two differences: disabled blending becomes 1 x source + 0 x
   * destination, ADD (0x00010001; with 0, FillFixedState would enable it with zero factors), and with
   * blending enabled the half of the equation (color or alpha) whose channels the mask does not write also
   * becomes that.
   */
  static void Canonicalize(PipelineKey& c) {
    constexpr uint32_t kDirect = 0x00010001u;  // 1 x source + 0 x destination, ADD, for color and alpha
    uint32_t masks = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (!c.formats[i]) {
        c.blend[i] = 0;  // FillFixedState skips targets not in the pass (Draw leaves them at 0)
        continue;
      }
      const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
      masks |= mask << (i * 4);
      const uint32_t m = c.blend[i];
      const bool color_direct = (m & 0x1F) == 1 && ((m >> 8) & 0x1F) == 0 && ((m >> 5) & 0x7) == 0;
      const bool direct_alpha = ((m >> 16) & 0x1F) == 1 && ((m >> 24) & 0x1F) == 0 && ((m >> 21) & 0x7) == 0;
      const bool writes_rgb = (mask & 0x7) != 0;
      const bool writes_alpha = (mask & 0x8) != 0;
      if (!((writes_rgb && !color_direct) || (writes_alpha && !direct_alpha))) {
        c.blend[i] = kDirect;  // without blendEnable Vulkan ignores the equation
        continue;
      }
      uint32_t canonical = m;
      if (!writes_rgb) {
        canonical = (canonical & 0xFFFF0000u) | 0x00000001u;  // the color equation reaches no channel
      }
      if (!writes_alpha) {
        canonical = (canonical & 0x0000FFFFu) | 0x00010000u;  // neither does the alpha one
      }
      c.blend[i] = canonical;
    }
    c.masks = masks;
    uint32_t d = c.formats[4] ? c.depth : 0;  // ignored without a depth target
    d &= ~0x8u;                                      // bit 3 is not read
    if (!((d >> 1) & 0x1)) {
      d &= ~0x74u;  // without a Z test, write and function do not count
    }
    if (!(d & 0x1)) {
      d &= 0x77u;  // without stencil, its functions and operations do not count
    } else if (!((d >> 7) & 0x1)) {
      d &= 0x000FFFF7u;  // without its own back face state, the back copies the front
    }
    c.depth = d;
  }

  /*
   * The phase 0a guard. Canonicalize only touches blending, masks and depth: the rest of the key must come
   * out the same, and the fixed state of both keys must match in everything Vulkan looks at
   * (EffectiveDifference on FillFixedState). On a difference the phase switches off for the session and
   * false is returned.
   */
  bool CheckCanonicalKey(const PipelineKey& raw, const PipelineKey& canonical, uint64_t n) {
    ++canonical_checked_;
    const char* field = nullptr;
    if (raw.vs != canonical.vs || raw.ps != canonical.ps || raw.entry != canonical.entry ||
        raw.topology != canonical.topology || raw.specialization != canonical.specialization ||
        raw.rasterization != canonical.rasterization || raw.fill != canonical.fill ||
        raw.fill2 != canonical.fill2 ||
        !Equal(raw.formats, canonical.formats, sizeof(raw.formats))) {
      field = "shaders, input, topology, specialization, rasterization or formats";
    } else {
      FixedPipelineState a;
      FixedPipelineState b;
      FillFixedState(raw, a);
      FillFixedState(canonical, b);
      field = EffectiveDifference(a, b);
    }
    if (field) {
      canonical_off_key_ = true;
      canonical_key_ = false;
      canonical_valid_ = false;
      REXLOG_ERROR("[native] C6 canonical key: MISMATCH in {} (check {}: VS {} PS {}, blend {:08X} -> "
                   "{:08X}, masks {:04X} -> {:04X}, depth {:08X} -> {:08X}). Switched off for the rest of the "
                   "session: lookup uses the usual key",
                   field, n, raw.vs, raw.ps, raw.blend[0], canonical.blend[0], raw.masks,
                   canonical.masks, raw.depth, canonical.depth);
      return false;
    }
    if (n == kCanonicalToCheck) {
      MASSEFFECT_REPORT_RING("[native] C6 canonical key: {} changed keys checked field by field against the "
                           "usual one, 0 mismatches; still checking 1 in 4096",
                           n);
    }
    return true;
  }

  /*
   * The key used to look up a draw's pipeline. The raw one (key) is still used for everything else: the
   * deferred sky reads its blending (opaque_in_all) and the counter compares it. Phase 0a: canonical
   * form, remembering the previous draw's (the same raw key gives the same canonical one).
   */
  PipelineKey SearchKey(const PipelineKey& key) {
    PipelineKey c = key;
    if (canonical_key_) {
      if (canonical_valid_ && Equal(&key, &canonical_raw_, sizeof(key))) {
        c = canonical_result_;
      } else {
        Canonicalize(c);
        if (!Equal(&c, &key, sizeof(c))) {
          const uint64_t n = ++canonical_changed_;
          if ((n <= kCanonicalToCheck || (n & 4095) == 0) && !CheckCanonicalKey(key, c, n)) {
            c = key;  // the guard saw a difference: this draw, and the rest of the session, use the usual one
          }
        }
        canonical_raw_ = key;
        canonical_result_ = c;
        canonical_valid_ = canonical_key_;
      }
    }
    // Phases 1 and 2. What goes through vkCmdSet* leaves the key, and the flag (fill2) keeps these
    // pipelines apart from the usual ones in the map, in the direct-mapped cache and in PipelineFor's
    // shortcut.
    const uint32_t eds_mode = eds_mode_;
    if (eds_mode & kEds12) {
      c.depth = 0;
      c.rasterization = me::native::StaticRasterKey(c.rasterization);
      c.topology = RepresentativeTopology(c.topology);
    }
    if (eds_mode & kEds3) {  // phase 2: blending and masks go through vkCmdSet*; targets stay in the formats part of the key
      std::fill(std::begin(c.blend), std::end(c.blend), 0u);
      c.masks = 0;
    }
    c.fill2 = eds_mode;
    return c;
  }

  // Every 20 s, phase 0a (masseffect_native_canonical_key).
  void ReportCanonicalKey() {
    const auto now = std::chrono::steady_clock::now();
    if (now - canonical_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = canonical_report_ == std::chrono::steady_clock::time_point{};
    canonical_report_ = now;
    const uint64_t changed = canonical_changed_ - canonical_changed_previous_;
    canonical_changed_previous_ = canonical_changed_;
    const uint64_t pipelines = pipelines_.size();
    const uint64_t new_values = pipelines - std::min<uint64_t>(pipelines, canonical_pipelines_previous_);
    canonical_pipelines_previous_ = pipelines;
    if (first || (!changed && !new_values)) {
      return;
    }
    MASSEFFECT_REPORT_RING("[native] C6 canonical key: {}; {} keys changed in 20 s ({} checked "
                         "since the start, {}); {} pipelines in the map, {} new in 20 s",
                         canonical_off_key_ ? "SWITCHED OFF by the guard"
                         : canonical_key_       ? "on"
                                                 : "off",
                         changed, canonical_checked_,
                         canonical_checked_ >= kCanonicalToCheck ? "guard passed" : "checking", pipelines,
                         new_values);
  }

  // Every 20 s, phase 0b (masseffect_native_pipeline_between_passes).
  void ReportPipelineBetweenPasses() {
    const auto now = std::chrono::steady_clock::now();
    if (now - between_passes_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = between_passes_report_ == std::chrono::steady_clock::time_point{};
    between_passes_report_ = now;
    const uint64_t passes = started_passes_ - between_passes_previous_passes_;
    const uint64_t kept = passes_with_pipeline_ - between_kept_previous_passes_;
    between_passes_previous_passes_ = started_passes_;
    between_kept_previous_passes_ = passes_with_pipeline_;
    if (first || !passes) {
      return;
    }
    MASSEFFECT_REPORT_RING("[native] C6 pipeline between passes: {}; {} passes started in 20 s, {} with a "
                         "bound pipeline that is kept (its first draw does not bind again if the key is the "
                         "same)",
                         pipeline_between_passes_ ? "kept" : "forgotten, as before", passes, kept);
  }

  /*
   * Dynamic state phases 1 and 2. The state that leaves the pipeline, with the Vulkan values the usual
   * pipeline would carry (the criterion of FillFixedState, which is PipelineFor's).
   */
  struct EdsState {
    uint32_t face = 0;         // VkCullModeFlags
    uint32_t front = 0;       // VkFrontFace
    uint32_t topology = 0;    // VkPrimitiveTopology
    uint32_t reset = 0;     // primitiveRestartEnable
    uint32_t bias = 0;        // depthBiasEnable
    uint32_t test_z = 0;     // depthTestEnable
    uint32_t writes_z = 0;    // depthWriteEnable
    uint32_t function_z = 0;    // VkCompareOp
    uint32_t stencil = 0;     // stencilTestEnable
    uint32_t ahead[4] = {};  // failOp, passOp, depthFailOp (VkStencilOp) and compareOp (VkCompareOp)
    uint32_t behind[4] = {};   // the same for the back face
    uint32_t n_colors = 0;                    // phase 2: the pass's color targets, compacted as in PipelineFor
    VkBool32 active_blend[4] = {};            // blendEnable
    VkColorBlendEquationEXT equation[4] = {};  // color and alpha factors and operations
    VkColorComponentFlags mask[4] = {};     // colorWriteMask
  };

  // Without EDS3 the dynamic topology must be of the same class as the pipeline's: the lookup key carries
  // one per class. Classes the ring does not draw stay as they are.
  static uint32_t RepresentativeTopology(uint32_t topology) {
    switch (ClassTopology(topology)) {
      case 1:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      case 2:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      default:
        return topology;
    }
  }

  static const char* NameEdsMode(uint32_t mode) {
    switch (mode & (kEds12 | kEds3)) {
      case kEds12:
        return "EDS1/EDS2 (phase 1)";
      case kEds3:
        return "EDS3 (phase 2)";
      case kEds12 | kEds3:
        return "EDS1/EDS2 and EDS3 (phases 1 and 2)";
      default:
        return "off: all state in the pipeline, as before";
    }
  }

  // The dynamic state of a raw key in the given mode (kEds12 | kEds3).
  void EdsStateFor(const PipelineKey& c, EdsState& e, uint32_t mode) {
    e = EdsState{};
    if (mode & kEds12) {
      e.face = ((c.rasterization & 0x1) ? uint32_t(VK_CULL_MODE_FRONT_BIT) : 0u) |
               ((c.rasterization & 0x2) ? uint32_t(VK_CULL_MODE_BACK_BIT) : 0u);
      e.front = (c.rasterization & 0x4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
      e.reset = (c.rasterization & 0x8) ? 1u : 0u;
      e.bias = (c.rasterization & 0x10) ? 1u : 0u;
      e.topology = c.topology;
      if (c.formats[4]) {  // without a depth target everything stays zero, as in FillFixedState
        const uint32_t d = c.depth;
        e.test_z = (d >> 1) & 0x1;
        e.writes_z = ((d >> 1) & 0x1) & ((d >> 2) & 0x1);
        e.function_z = (d >> 4) & 0x7;
        e.stencil = d & 0x1;
        e.ahead[0] = (d >> 11) & 0x7;
        e.ahead[1] = (d >> 14) & 0x7;
        e.ahead[2] = (d >> 17) & 0x7;
        e.ahead[3] = (d >> 8) & 0x7;
        if ((d >> 7) & 0x1) {
          e.behind[0] = (d >> 23) & 0x7;
          e.behind[1] = (d >> 26) & 0x7;
          e.behind[2] = (d >> 29) & 0x7;
          e.behind[3] = (d >> 20) & 0x7;
        } else {
          std::memcpy(e.behind, e.ahead, sizeof(e.behind));
        }
      }
    }
    if (mode & kEds3) {  // phase 2: like FillFixedState, target by target and compacted
      for (uint32_t i = 0; i < 4; ++i) {
        if (!c.formats[i]) {
          continue;
        }
        const uint32_t a = e.n_colors++;
        const uint32_t m = c.blend[i];
        const uint32_t mask = (c.masks >> (i * 4)) & 0xF;
        const uint32_t src_data = m & 0x1F, op = (m >> 5) & 0x7, target = (m >> 8) & 0x1F;
        const uint32_t source_a = (m >> 16) & 0x1F, op_a = (m >> 21) & 0x7, target_a = (m >> 24) & 0x1F;
        const bool color_direct = src_data == 1 && target == 0 && op == 0;
        const bool direct_alpha = source_a == 1 && target_a == 0 && op_a == 0;
        e.mask[a] = mask;
        e.active_blend[a] =
            (((mask & 0x7) != 0 && !color_direct) || ((mask & 0x8) != 0 && !direct_alpha)) ? VK_TRUE : VK_FALSE;
        VkColorBlendEquationEXT& q = e.equation[a];
        q.srcColorBlendFactor = kFactorsBlend[src_data];
        q.dstColorBlendFactor = kFactorsBlend[target];
        q.colorBlendOp = OperationBlend(op);
        q.srcAlphaBlendFactor = kFactorsBlend[source_a];
        q.dstAlphaBlendFactor = kFactorsBlend[target_a];
        q.alphaBlendOp = OperationBlend(op_a);
      }
    }
  }

  /*
   * Records the vkCmdSet* calls for what changed since the last state set in the buffer (eds_recorded_),
   * or for everything with all = true, and tracks it. What Vulkan ignores in this draw (the Z function
   * without a Z test, stencil operations without stencil, the blend equation without blending) is not
   * re-recorded if only that changed; with all = true it is recorded anyway, so everything is set at least
   * once in the buffer.
   */
  void EmitDynamicState(VkCommandBuffer cmd, const EdsState& d, bool all, uint32_t mode) {
    EdsState& g = eds_recorded_;
    eds_calls_[kEdsAll] += all ? 1 : 0;
    if (mode & kEds12) {
      if (all || d.face != g.face) {
        set_face_(cmd, VkCullModeFlags(d.face));
        g.face = d.face;
        ++eds_calls_[kEdsFace];
      }
      if (all || d.front != g.front) {
        set_front_(cmd, VkFrontFace(d.front));
        g.front = d.front;
        ++eds_calls_[kEdsFront];
      }
      if (all || d.topology != g.topology) {
        set_topology_(cmd, VkPrimitiveTopology(d.topology));
        g.topology = d.topology;
        ++eds_calls_[kEdsTopology];
      }
      if (all || d.reset != g.reset) {
        set_reset_(cmd, VkBool32(d.reset));
        g.reset = d.reset;
        ++eds_calls_[kEdsReset];
      }
      if (all || d.bias != g.bias) {
        set_bias_(cmd, VkBool32(d.bias));
        g.bias = d.bias;
        ++eds_calls_[kEdsBias];
      }
      if (all || d.test_z != g.test_z) {
        set_test_z_(cmd, VkBool32(d.test_z));
        g.test_z = d.test_z;
        ++eds_calls_[kEdsTestZ];
      }
      if (all || d.writes_z != g.writes_z) {
        set_writes_z_(cmd, VkBool32(d.writes_z));
        g.writes_z = d.writes_z;
        ++eds_calls_[kEdsWritesZ];
      }
      if (all || (d.test_z && d.function_z != g.function_z)) {
        set_function_z_(cmd, VkCompareOp(d.function_z));
        g.function_z = d.function_z;
        ++eds_calls_[kEdsFunctionZ];
      }
      if (all || d.stencil != g.stencil) {
        set_stencil_(cmd, VkBool32(d.stencil));
        g.stencil = d.stencil;
        ++eds_calls_[kEdsStencil];
      }
      const bool ahead = all || (d.stencil && !Equal(d.ahead, g.ahead, sizeof(d.ahead)));
      const bool behind = all || (d.stencil && !Equal(d.behind, g.behind, sizeof(d.behind)));
      if (ahead && behind && Equal(d.ahead, d.behind, sizeof(d.ahead))) {
        set_stencil_ops_(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VkStencilOp(d.ahead[0]), VkStencilOp(d.ahead[1]),
                          VkStencilOp(d.ahead[2]), VkCompareOp(d.ahead[3]));
        ++eds_calls_[kEdsStencilOps];
      } else {
        if (ahead) {
          set_stencil_ops_(cmd, VK_STENCIL_FACE_FRONT_BIT, VkStencilOp(d.ahead[0]), VkStencilOp(d.ahead[1]),
                            VkStencilOp(d.ahead[2]), VkCompareOp(d.ahead[3]));
          ++eds_calls_[kEdsStencilOps];
        }
        if (behind) {
          set_stencil_ops_(cmd, VK_STENCIL_FACE_BACK_BIT, VkStencilOp(d.behind[0]), VkStencilOp(d.behind[1]),
                            VkStencilOp(d.behind[2]), VkCompareOp(d.behind[3]));
          ++eds_calls_[kEdsStencilOps];
        }
      }
      if (ahead) {
        std::memcpy(g.ahead, d.ahead, sizeof(g.ahead));
      }
      if (behind) {
        std::memcpy(g.behind, d.behind, sizeof(g.behind));
      }
    }
    if (mode & kEds3) {  // phase 2
      const uint32_t n = d.n_colors;
      if (!n) {
        if (all) {
          g.n_colors = 0;  // nothing set in this buffer: the first draw with color will set it all
        }
      } else {
        const bool other_n = all || n != g.n_colors;
        bool active = other_n;
        bool equation = other_n;
        bool mask = other_n;
        for (uint32_t a = 0; a < n && a < 4; ++a) {
          active = active || d.active_blend[a] != g.active_blend[a];
          mask = mask || d.mask[a] != g.mask[a];
          if (!equation && d.active_blend[a]) {  // without blending the equation does not count; each half, with its channels
            const VkColorBlendEquationEXT& x = d.equation[a];
            const VkColorBlendEquationEXT& y = g.equation[a];
            equation = ((d.mask[a] & 0x7) != 0 &&
                        (x.srcColorBlendFactor != y.srcColorBlendFactor ||
                         x.dstColorBlendFactor != y.dstColorBlendFactor || x.colorBlendOp != y.colorBlendOp)) ||
                       ((d.mask[a] & 0x8) != 0 &&
                        (x.srcAlphaBlendFactor != y.srcAlphaBlendFactor ||
                         x.dstAlphaBlendFactor != y.dstAlphaBlendFactor || x.alphaBlendOp != y.alphaBlendOp));
          }
        }
        if (active) {
          set_active_blend_(cmd, 0, n, d.active_blend);
          std::memcpy(g.active_blend, d.active_blend, sizeof(g.active_blend));
          ++eds_calls_[kEdsActiveBlend];
        }
        if (equation) {
          set_equation_(cmd, 0, n, d.equation);
          std::memcpy(g.equation, d.equation, sizeof(g.equation));
          ++eds_calls_[kEdsEquation];
        }
        if (mask) {
          set_mask_(cmd, 0, n, d.mask);
          std::memcpy(g.mask, d.mask, sizeof(g.mask));
          ++eds_calls_[kEdsMask];
        }
        g.n_colors = n;
      }
    }
  }

  /*
   * The guard of phases 1 and 2. What the ring believes is set in the buffer (eds_recorded_) must be what
   * this draw's usual pipeline would carry. That state comes from FillFixedState with the raw key, the
   * same code PipelineFor uses to create the usual pipelines (not another copy of EdsStateFor), and it is
   * compared field by field in what Vulkan looks at. On a difference dynamic state switches off for the
   * session (both phases) and false is returned: the draw has to use its usual pipeline. Limit: it cannot
   * see the GPU, only that the computations and the record of what was recorded agree.
   */
  bool CheckDynamicState(const PipelineKey& key, uint64_t n) {
    ++eds_checked_;
    FixedPipelineState fixed;
    FillFixedState(key, fixed);
    const EdsState& g = eds_recorded_;
    const char* field = nullptr;
    uint32_t pinned = 0;
    uint32_t expected = 0;
    const auto look = [&](const char* name, uint32_t a, uint32_t b) {
      if (!field && a != b) {
        field = name;
        pinned = a;
        expected = b;
      }
    };
    if (eds_mode_ & kEds12) {
      const VkPipelineRasterizationStateCreateInfo& r = fixed.rasterization;
      const VkPipelineDepthStencilStateCreateInfo& z = fixed.depth;
      look("cullMode", g.face, r.cullMode);
      look("frontFace", g.front, r.frontFace);
      look("topology", g.topology, fixed.assembly.topology);
      look("primitiveRestartEnable", g.reset, fixed.assembly.primitiveRestartEnable);
      look("depthBiasEnable", g.bias, r.depthBiasEnable);
      look("depthTestEnable", g.test_z, z.depthTestEnable);
      look("depthWriteEnable", g.writes_z, z.depthWriteEnable);
      if (z.depthTestEnable) {
        look("depthCompareOp", g.function_z, z.depthCompareOp);
      }
      look("stencilTestEnable", g.stencil, z.stencilTestEnable);
      if (z.stencilTestEnable) {
        look("front.failOp", g.ahead[0], z.front.failOp);
        look("front.passOp", g.ahead[1], z.front.passOp);
        look("front.depthFailOp", g.ahead[2], z.front.depthFailOp);
        look("front.compareOp", g.ahead[3], z.front.compareOp);
        look("back.failOp", g.behind[0], z.back.failOp);
        look("back.passOp", g.behind[1], z.back.passOp);
        look("back.depthFailOp", g.behind[2], z.back.depthFailOp);
        look("back.compareOp", g.behind[3], z.back.compareOp);
      }
    }
    if ((eds_mode_ & kEds3) && fixed.n_colors) {  // phase 2 (without color targets there is nothing to check)
      look("attachmentCount", g.n_colors, fixed.n_colors);
      for (uint32_t a = 0; a < fixed.n_colors && a < 4; ++a) {
        const VkPipelineColorBlendAttachmentState& s = fixed.blends[a];
        const VkColorBlendEquationEXT& q = g.equation[a];
        look("colorWriteMask", g.mask[a], s.colorWriteMask);
        look("blendEnable", g.active_blend[a], s.blendEnable);
        if (s.blendEnable && (s.colorWriteMask & 0x7)) {
          look("srcColorBlendFactor", q.srcColorBlendFactor, s.srcColorBlendFactor);
          look("dstColorBlendFactor", q.dstColorBlendFactor, s.dstColorBlendFactor);
          look("colorBlendOp", q.colorBlendOp, s.colorBlendOp);
        }
        if (s.blendEnable && (s.colorWriteMask & 0x8)) {
          look("srcAlphaBlendFactor", q.srcAlphaBlendFactor, s.srcAlphaBlendFactor);
          look("dstAlphaBlendFactor", q.dstAlphaBlendFactor, s.dstAlphaBlendFactor);
          look("alphaBlendOp", q.alphaBlendOp, s.alphaBlendOp);
        }
      }
    }
    if (field) {
      eds_off_ = true;
      eds_mode_ = 0;
      eds_valid_ = false;
      pipeline_bound_ = VK_NULL_HANDLE;
      REXLOG_ERROR("[native] C6 dynamic state: MISMATCH in {} (check {}: pinned {} and the usual pipeline "
                   "would have {}; VS {} PS {}, topology {}, depth {:08X}, rasterization {:02X}, masks {:04X}). "
                   "Switched off for the rest of the session: pipelines with all state fixed return",
                   field, n, pinned, expected, key.vs, key.ps, key.topology, key.depth,
                   key.rasterization, key.masks);
      return false;
    }
    if (n == kEdsToCheck) {
      MASSEFFECT_REPORT_RING("[native] C6 dynamic state: {} key changes checked against the state of their "
                           "usual pipeline, 0 mismatches; still checking 1 in 4096",
                           n);
    }
    return true;
  }

  /*
   * Phases 1 and 2, before every draw with dynamic state. With the same raw key as the last state set in
   * this buffer there is nothing to do; otherwise what changed is set and the guard checks it for the
   * first 200,000 changes and then 1 in 4,096. Returns false if the guard has switched dynamic state off.
   */
  bool PinDynamicState(VkCommandBuffer cmd, const PipelineKey& key) {
    ++eds_draws_;
    if (eds_valid_ && Equal(&key, &eds_key_, sizeof(key))) {
      ++eds_repeated_;
      return true;
    }
    EdsState d;
    EdsStateFor(key, d, eds_mode_);
    EmitDynamicState(cmd, d, !eds_valid_, eds_mode_);
    eds_key_ = key;
    eds_valid_ = true;
    const uint64_t n = ++eds_checkable_;
    if (n <= kEdsToCheck || (n & 4095) == 0) {
      return CheckDynamicState(key, n);
    }
    return true;
  }

  /*
   * Dynamic state phases 1 and 2. The EDS1/EDS2 vkCmdSet* functions are core in Vulkan 1.3 and the SDK's
   * table does not load them: they are requested from the driver, like vkCmdCopyImage. With an API below
   * 1.3 or with any of them missing, phase 1 is not used for the whole session and one log line says so.
   */
  void LoadDynamicState() {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    const auto request = [&](const char* name) { return deferred::Proc(ifn.vkGetDeviceProcAddr, device_, name); };
    set_face_ = reinterpret_cast<PFN_vkCmdSetCullMode>(request("vkCmdSetCullMode"));
    set_front_ = reinterpret_cast<PFN_vkCmdSetFrontFace>(request("vkCmdSetFrontFace"));
    set_topology_ = reinterpret_cast<PFN_vkCmdSetPrimitiveTopology>(request("vkCmdSetPrimitiveTopology"));
    set_reset_ = reinterpret_cast<PFN_vkCmdSetPrimitiveRestartEnable>(request("vkCmdSetPrimitiveRestartEnable"));
    set_bias_ = reinterpret_cast<PFN_vkCmdSetDepthBiasEnable>(request("vkCmdSetDepthBiasEnable"));
    set_test_z_ = reinterpret_cast<PFN_vkCmdSetDepthTestEnable>(request("vkCmdSetDepthTestEnable"));
    set_writes_z_ = reinterpret_cast<PFN_vkCmdSetDepthWriteEnable>(request("vkCmdSetDepthWriteEnable"));
    set_function_z_ = reinterpret_cast<PFN_vkCmdSetDepthCompareOp>(request("vkCmdSetDepthCompareOp"));
    set_stencil_ = reinterpret_cast<PFN_vkCmdSetStencilTestEnable>(request("vkCmdSetStencilTestEnable"));
    set_stencil_ops_ = reinterpret_cast<PFN_vkCmdSetStencilOp>(request("vkCmdSetStencilOp"));
    const uint32_t api = vulkan_device_->properties().apiVersion;
    eds12_available_ = api >= VK_MAKE_API_VERSION(0, 1, 3, 0) && set_face_ && set_front_ && set_topology_ &&
                        set_reset_ && set_bias_ && set_test_z_ && set_writes_z_ && set_function_z_ &&
                        set_stencil_ && set_stencil_ops_;
    REXLOG_INFO("[native] C6 dynamic state: EDS1/EDS2 {} (device API {}.{}.{})",
                eds12_available_ ? "available (core 1.3)" : "NOT available: phase 1 is not used",
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api));
    // Phase 2. The driver only provides these three if the SDK enabled the extension; the SDK records the
    // three features in its properties.
    set_active_blend_ = reinterpret_cast<PFN_vkCmdSetColorBlendEnableEXT>(request("vkCmdSetColorBlendEnableEXT"));
    set_equation_ = reinterpret_cast<PFN_vkCmdSetColorBlendEquationEXT>(request("vkCmdSetColorBlendEquationEXT"));
    set_mask_ = reinterpret_cast<PFN_vkCmdSetColorWriteMaskEXT>(request("vkCmdSetColorWriteMaskEXT"));
    const auto& p3 = vulkan_device_->properties();
    eds3_available_ = vulkan_device_->extensions().ext_EXT_extended_dynamic_state3 &&
                       p3.extendedDynamicState3ColorBlendEnable && p3.extendedDynamicState3ColorBlendEquation &&
                       p3.extendedDynamicState3ColorWriteMask && set_active_blend_ && set_equation_ && set_mask_;
    REXLOG_INFO("[native] C6 dynamic state: EDS3 (blend, equation and color mask) {}",
                eds3_available_ ? "available"
                                 : "NOT available (the device does not provide it or it is not enabled): phase 2 is not used");
  }

  // Every 20 s, dynamic state phases 1 and 2.
  void ReportDynamicState() {
    const auto now = std::chrono::steady_clock::now();
    if (now - eds_report_ < std::chrono::seconds(20)) {
      return;
    }
    const bool first = eds_report_ == std::chrono::steady_clock::time_point{};
    eds_report_ = now;
    const std::array<uint64_t, kEdsN> n = eds_calls_;
    eds_calls_.fill(0);
    const uint64_t draws = eds_draws_ - eds_previous_draws_;
    const uint64_t repeated = eds_repeated_ - eds_repeated_previous_;
    const uint64_t frames = frame_ - eds_previous_frames_;
    eds_previous_draws_ = eds_draws_;
    eds_repeated_previous_ = eds_repeated_;
    eds_previous_frames_ = frame_;
    if (first || !draws) {
      return;
    }
    uint64_t calls = 0;
    for (size_t k = 0; k < kEdsN; ++k) {
      calls += k == kEdsAll ? 0 : n[k];
    }
    MASSEFFECT_REPORT_RING(
        "[native] C6 dynamic state: {}; {} draws in 20 s ({:.0f} per frame; {} repeat the previous key) "
        "and {} vkCmdSet* ({:.3f} per draw; {} times everything: new buffer, sky or pass that forgets the "
        "pipeline) | face {}, front {}, topology {}, restart {}, bias {}, Z test {}, Z write {}, Z function "
        "{}, stencil {}, stencil operations {} | blend {}, equation {}, masks {} | pipelines created: {} "
        "regular, {} EDS1/EDS2, {} EDS3, {} with both | guard: {} checked ({})",
        eds_off_ ? "SWITCHED OFF by the guard" : NameEdsMode(eds_mode_), draws,
        frames ? double(draws) / double(frames) : 0.0, repeated, calls,
        double(calls) / double(draws), n[kEdsAll], n[kEdsFace], n[kEdsFront], n[kEdsTopology],
        n[kEdsReset], n[kEdsBias], n[kEdsTestZ], n[kEdsWritesZ], n[kEdsFunctionZ], n[kEdsStencil],
        n[kEdsStencilOps], n[kEdsActiveBlend], n[kEdsEquation], n[kEdsMask], pipelines_per_mode_[0],
        pipelines_per_mode_[kEds12], pipelines_per_mode_[kEds3], pipelines_per_mode_[kEds12 | kEds3],
        eds_checked_, eds_checked_ >= kEdsToCheck ? "guard passed" : "checking");
  }

  // Phase 2. The blend operation of a register field, like the lambda in FillFixedState.
  VkBlendOp OperationBlend(uint32_t op) {
    switch (op) {
      case 1:
        return VK_BLEND_OP_SUBTRACT;
      case 2:
        return VK_BLEND_OP_MIN;
      case 3:
        return VK_BLEND_OP_MAX;
      case 4:
        Warn(52, "blend with reverse subtract: subtract is used");
        return VK_BLEND_OP_SUBTRACT;
      default:
        return VK_BLEND_OP_ADD;
    }
  }

  VkPipeline PipelineFor(const PipelineKey& key, const VerticesEntry& entry,
                        const SubmissionDraw& p) {
    /*
     * One-entry shortcut.
     *
     * There are 105 pipelines in a whole session and 2,345 draws per frame, so consecutive draws almost always
     * repeat the key. The full lookup is an XXH3 of 80 bytes + an integer division by libstdc++'s prime
     * bucket count + two pointer hops (bucket and node, ~96 B) = 2-3 cache misses, all to end up comparing
     * the same 80 bytes this compares. Here there is just one memcmp on memory that is already hot.
     */
    if (last_valid_key_ && Equal(&last_key_, &key, sizeof(key))) {
      return last_pipeline_;
    }
    const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
    // The direct-mapped cache (masseffect_native_pipelines_direct) before the map.
    CellPipeline& cell = cells_pipeline_[fingerprint & (kCellsPipeline - 1)];
    if (pipelines_direct_ && cell.pipeline != VK_NULL_HANDLE &&
        Equal(&cell.key, &key, sizeof(key))) {
      const uint64_t n = ++pipelines_direct_hits_;
      if (!(n <= kPipelinesToCheck || (n & 4095) == 0) ||
          CheckCellPipeline(fingerprint, key, cell.pipeline, n)) {
        last_key_ = key;
        last_pipeline_ = cell.pipeline;
        last_valid_key_ = true;
        return cell.pipeline;
      }
      // the guard saw a difference: this draw goes through the map, as before
    }
    if (pipelines_direct_) {
      ++pipelines_direct_failures_;
    }
    if (const auto it = pipelines_.find(fingerprint); it != pipelines_.end()) {
      if (Equal(&it->second.first, &key, sizeof(key))) {
        last_key_ = key;
        last_pipeline_ = it->second.second;
        last_valid_key_ = true;
        if (pipelines_direct_) {  // the slot keeps the map's pair
          cell.key = key;
          cell.pipeline = it->second.second;
        }
        return it->second.second;
      }
      Warn(50, "pipeline fingerprint collision");
      return VK_NULL_HANDLE;
    }
    VkShaderModule vs = (key.specialization & kSpecRectangle) ? ModuleRectangle(*p.vs) : ModuleFor(*p.vs);
    VkShaderModule ps = VK_NULL_HANDLE;
    if (key.ps && (key.specialization & kSpecTarget7e3)) {
      const uint32_t mask_7e3 =
          (key.specialization & kSpecMask7e3) >> kSpecMask7e3Displacement;
      ps = Module7e3(*p.ps, mask_7e3);
    } else if (key.ps && (key.specialization & kSpecAlphaOnly)) {
      ps = ModuleAlphaOnly(*p.ps);
    } else if (key.ps && (key.specialization & kSpecZEarly)) {
      ps = ModuleZEarly(*p.ps);
    } else if (key.ps) {
      ps = ModuleFor(*p.ps);
    }
    if (key.ps && ((key.specialization & kSpecRasterGridX) ||
                    (key.rasterization & me::native::kNativeMsaa2PhaseProbe))) {
      ps = ModuleGuestFragCoordXY(ps, key, p.vs, p.ps);
      if (!ps) {
        Reject(51, "PS not supported by guest FragCoord XY remapping");
        return VK_NULL_HANDLE;
      }
    }
    if (key.specialization & kSpecDepthFloat24Quantize)
      ps = ModuleDepthQuantize(ps, bool(key.specialization & kSpecDepthFloat24Round), !key.ps);
    else if (key.ps && (key.specialization & kSpecDepthFloat24Half)) ps = ModuleDepthHalf(ps);
    if (vs == VK_NULL_HANDLE || ((key.ps || (key.specialization & kSpecDepthFloat24Quantize)) && ps == VK_NULL_HANDLE)) {
      Reject(51, "could not create a shader module");
      return VK_NULL_HANDLE;
    }
    // The VkGraphicsPipelineCreateInfo lives in CreatePipelineVulkan, the same function the prewarm uses from
    // its thread; what surrounds it stays here, as it was.
    uint32_t n_colors = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const auto creation_start = std::chrono::steady_clock::now();
    if (CreatePipelineVulkan(key, entry, vs, ps, pass_rp_, true, pipeline, n_colors) != VK_SUCCESS) {
      Reject(53, "could not create a pipeline");
      pipeline = VK_NULL_HANDLE;
    } else {
      ++pipelines_no_save_;
    }
    const uint64_t ns_creation = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now() - creation_start)
                                              .count());
    ns_pipelines_ += ns_creation;
    NotePipelineCreated(key, entry, p, pipeline != VK_NULL_HANDLE, ns_creation);  // List and measurement
    pipelines_.emplace(fingerprint, std::make_pair(key, pipeline));
    ++pipelines_per_mode_[key.fill2 & 3];  // Dynamic state report
    if (pipelines_direct_ && pipeline != VK_NULL_HANDLE) {  // the same pair as the map
      cell.key = key;
      cell.pipeline = pipeline;
    }
    if (pipelines_.size() <= 64) {
      REXLOG_INFO("[native] C6: pipeline {} (VS n{} fingerprint {:016X} PS n{} fingerprint {:016X}, topology {}, {} colors, "
                  "depth {:08X}, blend {:08X}, specialization {})",
                  pipelines_.size(), p.vs->number, p.vs->fingerprint, key.ps ? int(p.ps->number) : -1,
                  key.ps ? p.ps->fingerprint : 0, key.topology, n_colors,
                  key.depth, key.blend[0], key.specialization);
    }
    return pipeline;
  }

  /*
   * The Vulkan pipeline for a key. It is PipelineFor's usual code, moved here without changes except for
   * the render pass (now an argument) and warn, so the prewarm can create from its thread exactly the
   * same VkGraphicsPipelineCreateInfo as the ring. It only reads its arguments, layout_pipeline_ and the
   * pipeline cache, which Vulkan synchronizes internally; warn = false outside the ring.
   */
  VkResult CreatePipelineVulkan(const PipelineKey& key, const VerticesEntry& entry, VkShaderModule vs,
                               VkShaderModule ps, VkRenderPass pass, bool warn, VkPipeline& pipeline,
                               uint32_t& n_colors_output) {
    const VkSpecializationMapEntry map{0, 0, sizeof(uint32_t)};
    const VkSpecializationInfo specialization{1, &map, sizeof(uint32_t), &key.specialization};
    // Vertex stage: also constant 60 = the outputs the pixel shader reads (ignored by older packages).
    // No fragment shader (depth-only draws): no output but the position is read.
    const uint32_t data_vs[2] = {key.specialization,
        ps == VK_NULL_HANDLE && REXCVAR_GET(masseffect_native_vs_pruned_outputs) ? 0u : InputsMaskPS(ps)};
    const VkSpecializationMapEntry map_vs[2] = {{0, 0, sizeof(uint32_t)}, {60, sizeof(uint32_t), sizeof(uint32_t)}};
    const VkSpecializationInfo specialization_vs{2, map_vs, sizeof(data_vs), data_vs};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) {
      stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stage.pName = "main";
      stage.pSpecializationInfo = &specialization;
    }
    stages[0].pSpecializationInfo = &specialization_vs;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;

    std::vector<VkVertexInputBindingDescription> bindings;
    for (uint32_t i = 0; i < entry.bindings.size(); ++i) {
      bindings.push_back({i, entry.bindings[i].stride, VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const AttributeVertices& a : entry.attributes) {
      attributes.push_back({a.location, a.binding, a.format, a.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertices{};
    vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertices.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vertices.pVertexBindingDescriptions = bindings.data();
    vertices.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertices.pVertexAttributeDescriptions = attributes.data();

    // The fixed state is filled in by FillFixedState: it is the usual code, moved there without
    // changing any computation, so the guards of the canonical key and of dynamic state compare with what
    // really goes into the VkGraphicsPipelineCreateInfo.
    FixedPipelineState fixed;
    FillFixedState(key, fixed, warn);  // notify
    const VkPipelineInputAssemblyStateCreateInfo& assembly = fixed.assembly;
    const VkPipelineViewportStateCreateInfo& view = fixed.view;
    const VkPipelineRasterizationStateCreateInfo& rasterization = fixed.rasterization;
    const VkPipelineMultisampleStateCreateInfo& sampling = fixed.sampling;
    const VkPipelineDepthStencilStateCreateInfo& depth = fixed.depth;
    const VkPipelineColorBlendStateCreateInfo& blend = fixed.blend;
    const uint32_t n_colors = fixed.n_colors;

    // Dynamic state phases 1 and 2. Their pipelines (flagged in fill2) also declare as dynamic what is
    // now set with vkCmdSet*; their lookup key already has it zeroed (SearchKey).
    VkDynamicState dynamic_offsets[24] = {
        VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS};
    uint32_t n_dynamic = 7;
    if (key.fill2 & kEds12) {
      for (const VkDynamicState state : kDynamicEds12) {
        dynamic_offsets[n_dynamic++] = state;
      }
    }
    if (key.fill2 & kEds3) {
      for (const VkDynamicState state : kDynamicEds3) {
        dynamic_offsets[n_dynamic++] = state;
      }
    }
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = n_dynamic;
    dynamic.pDynamicStates = dynamic_offsets;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = (key.ps || (key.specialization & kSpecDepthFloat24Quantize)) ? 2 : 1;
    info.pStages = stages;
    info.pVertexInputState = &vertices;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &view;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &sampling;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_pipeline_;
    info.renderPass = pass;  // the pass's (ring) or a compatible one (prewarm)
    info.basePipelineIndex = -1;
    n_colors_output = n_colors;
    pipeline = VK_NULL_HANDLE;
    return dfn_.vkCreateGraphicsPipelines(device_, cache_pipelines_, 1, &info, nullptr, &pipeline);
  }

  // --- Pipeline prewarming (masseffect_native_pipelines_prewarm) ---------------------------
  /*
   * Why. A pipeline that is not in the Vulkan cache is compiled on the fly on the ring: 68-159 ms each on
   * the console. In a first run after the library and the key had changed there were 56 (5.2 s of
   * stutter at the start). It happens the first time after any change to the library, the driver or the
   * key, and on a fresh install.
   *
   * How. The ring records every pipeline it creates (NotePipelineCreated) and the list is saved with the
   * cache (SaveCachePipelines). In the next session, as soon as the library is loaded
   * (TryPrewarm), a lowest-priority thread (PrewarmedLoop) walks the list in creation order
   * (the menu first) and recreates each pipeline with CreatePipelineVulkan, the same function the ring
   * uses, with modules from the same SPIR-V (the same variants and fallbacks as PipelineFor) and a
   * compatible render pass (CreatePass, the same formats); then destroys it. The Vulkan cache keeps the
   * compiled shaders, so when the ring asks for it, it comes from the cache (0-2 ms, like the 8 of the
   * menu). The ring changes nothing of what it draws: it only reads two counters of the thread for the
   * report and the guard.
   *
   * Guard. The thread's pipelines are destroyed unused: they cannot change the image. What can fail is
   * that they are not the ones the ring asks for. The report measures how long the ring takes to create
   * the listed pipelines the thread already prewarmed: if at least half of 8 or more take as long as a
   * compiled one (20 ms or more), it writes MISMATCH and stops the thread. Records whose shaders are no
   * longer in the library, or from another dynamic state mode, are skipped.
   */
  enum : uint8_t { kPendingList = 0, kPrewarmedList = 1, kListNoShader = 2, kFailedList = 3,
                   kOtherListMode = 4 };
  static constexpr uint64_t kNsCompiled = 5000000;  // more than this: really compiled (a cache hit is 0-2 ms)
  static constexpr uint64_t kNsSlow = 20000000;     // the ring creating one already prewarmed: mismatch

  // At start-up (Initialize): the previous session's list, before the thread exists. It is not a
  // separate file: it is the part of the pipelines file that LoadCachePipelines read.
  void LoadPipelinesList() {
    const std::filesystem::path path = PathFilePipelines();
    std::vector<uint8_t> data = std::move(read_list_);
    read_list_ = {};
    if (data.size() < kHeaderList ||
        data.size() > kHeaderList + kMaxRegistersList * sizeof(RegisterPipeline)) {
      data.clear();
    }
    const char* reason = nullptr;
    uint32_t header[4] = {};
    if (data.empty()) {
      reason = "no list: starting fresh in this session";
    } else {
      std::memcpy(header, data.data(), sizeof(header));
      if (header[0] != kMagicPipelinesList || header[1] != kVersionPipelinesList ||
          header[2] != sizeof(RegisterPipeline) ||
          data.size() != kHeaderList + size_t(header[3]) * sizeof(RegisterPipeline)) {
        reason = "list from another version or damaged: starting from scratch";
      }
    }
    if (!reason) {
      file_list_.reserve(header[3]);
      for (uint32_t i = 0; i < header[3]; ++i) {
        RegisterPipeline r;
        std::memcpy(&r, data.data() + kHeaderList + size_t(i) * sizeof(RegisterPipeline), sizeof(r));
        if (r.n_attributes > RegisterPipeline::kMaxAttributes || r.n_bindings > RegisterPipeline::kMaxBindings) {
          continue;
        }
        if (index_list_.emplace(XXH3_64bits(&r.key, sizeof(r.key)), file_list_.size()).second) {
          file_list_.push_back(r);
        }
      }
      written_list_ = std::move(data);  // the writer thread rewrites it unchanged if only the cache changes
    }
    state_list_.assign(file_list_.size(), kPendingList);
    REXLOG_INFO("[native] C6 prewarm: {} pipelines in the list from {}{}{}", file_list_.size(),
                path.string(), reason ? ": " : "", reason ? reason : "");
  }

  // Ring only (SaveCachePipelines): the file's list without the records the thread found missing their
  // shaders, followed by this session's new ones. Above kMaxRegistersList the oldest are dropped.
  std::vector<uint8_t> SerializePipelinesList() const {
    const size_t until = prewarmed_until_.load(std::memory_order_acquire);
    std::vector<const RegisterPipeline*> registers;
    registers.reserve(file_list_.size() + session_list_.size());
    for (size_t i = 0; i < file_list_.size(); ++i) {
      if (i < until && state_list_[i] == kListNoShader) {
        continue;
      }
      registers.push_back(&file_list_[i]);
    }
    for (const RegisterPipeline& r : session_list_) {
      registers.push_back(&r);
    }
    if (registers.size() > kMaxRegistersList) {
      registers.erase(registers.begin(), registers.begin() + std::ptrdiff_t(registers.size() - kMaxRegistersList));
    }
    std::vector<uint8_t> data(kHeaderList + registers.size() * sizeof(RegisterPipeline));
    const uint32_t header[4] = {kMagicPipelinesList, kVersionPipelinesList, uint32_t(sizeof(RegisterPipeline)),
                                  uint32_t(registers.size())};
    std::memcpy(data.data(), header, sizeof(header));
    for (size_t i = 0; i < registers.size(); ++i) {
      std::memcpy(data.data() + kHeaderList + i * sizeof(RegisterPipeline), registers[i], sizeof(RegisterPipeline));
    }
    return data;
  }

  // Ring only (PipelineFor), with every pipeline it creates: the measurement for the report and the guard,
  // and new ones go to the list.
  void NotePipelineCreated(const PipelineKey& key, const VerticesEntry& entry, const SubmissionDraw& p,
                            bool created, uint64_t ns) {
    const uint64_t fingerprint = XXH3_64bits(&key, sizeof(key));
    if (const auto it = index_list_.find(fingerprint); it != index_list_.end()) {
      const size_t j = it->second;
      if (j < file_list_.size() && j < prewarmed_until_.load(std::memory_order_acquire) &&
          state_list_[j] == kPrewarmedList) {
        ++prewarmed_ring_;
        ns_prewarmed_ring_ += ns;
        if (ns >= kNsSlow) {
          ++prewarmed_slow_ring_;
        }
      } else {
        ++ring_of_list_;
        ns_ring_of_list_ += ns;
      }
      return;
    }
    ++new_ring_;
    ns_new_ring_ += ns;
    if (!created || !p.vs || !p.vs->shader || (key.ps && (!p.ps || !p.ps->shader)) ||
        entry.attributes.size() > RegisterPipeline::kMaxAttributes ||
        entry.bindings.size() > RegisterPipeline::kMaxBindings ||
        file_list_.size() + session_list_.size() >= kMaxRegistersList) {
      return;
    }
    RegisterPipeline r;
    r.key = key;
    r.vs_fingerprint = p.vs->shader->fingerprint;
    r.ps_fingerprint = key.ps ? p.ps->shader->fingerprint : 0;
    r.n_attributes = uint32_t(entry.attributes.size());
    for (uint32_t k = 0; k < r.n_attributes; ++k) {
      const AttributeVertices& a = entry.attributes[k];
      r.attributes[k] = {a.location, a.binding, uint32_t(a.format), a.offset};
    }
    r.n_bindings = uint32_t(entry.bindings.size());
    for (uint32_t k = 0; k < r.n_bindings; ++k) {
      r.strides[k] = entry.bindings[k].stride;
    }
    index_list_.emplace(fingerprint, SIZE_MAX);  // from this session: not recorded again
    session_list_.push_back(r);
    ++list_no_save_;
    ++ring_to_list_;
  }

  // Shader prefetch (masseffect_shaders_preload). Ring only, on each submission until decided: with an indexed
  // library (SPIR-V read on first use), a background thread reads now the SPIR-V of every shader in the prewarm
  // list, so the ring does not read the SD card when it first needs them. It uses a file handle of its own;
  // other threads only wait for it while it publishes one entry.
  void TryPreloadShaders() {
    if (decided_preload_) {
      return;
    }
    if (!REXCVAR_GET(masseffect_shaders_preload)) {
      decided_preload_ = true;
      return;
    }
    const ShadersNative* library = ActiveLibrary();
    if (!library || !library->loaded()) {
      return;  // not yet
    }
    decided_preload_ = true;
    if (!library->is_indexed() || file_list_.empty()) {
      return;  // the whole package is resident, or there is nothing to prefetch
    }
    std::vector<uint64_t> fingerprints;
    fingerprints.reserve(file_list_.size() * 2);
    for (const RegisterPipeline& r : file_list_) {
      for (const auto [fingerprint, number] : {std::pair{r.vs_fingerprint, r.key.vs}, std::pair{r.ps_fingerprint, r.key.ps}}) {
        if (fingerprint) {
          fingerprints.push_back(fingerprint);
        } else if (number) {  // records written before the fingerprints (PrewarmedLoop does the same)
          if (const ShaderEntry* e = library->PerNumber(number - 1); e && e->shader) {
            fingerprints.push_back(e->shader->fingerprint);
          }
        }
      }
    }
    std::sort(fingerprints.begin(), fingerprints.end());
    fingerprints.erase(std::unique(fingerprints.begin(), fingerprints.end()), fingerprints.end());
    try {
      preload_thread_ = std::thread([this, library, fingerprints = std::move(fingerprints)] {
        rex::thread::set_current_thread_name("ME shader preload");
        // Low priority, never above the guest's (0x3B), like the prewarm thread: it only reads the SD card.
        for (const int32_t candidate : {0x3F, 0x3E, 0x3D, 0x3C, 0x3B}) {
          if (RexSwitchSetCurrentThreadPriorityOk(int(candidate))) {
            break;
          }
        }
        const auto start = std::chrono::steady_clock::now();
        const masseffect::native::StatsPreload r = library->PreloadSpirv(fingerprints, &preload_stop_);
        REXLOG_INFO("[native] U1 shader preload: {} entries read ({:.1f} MB of SPIR-V) and {} already resident out of "
                    "{} fingerprints from the pipeline list, in {} ms{}{}",
                    r.inputs, double(r.bytes) / (1024.0 * 1024.0), r.already_resident, fingerprints.size(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
                        .count(),
                    r.error ? "; read or verification ERROR, stopped" : "",
                    preload_stop_.load(std::memory_order_relaxed) ? "; stopped" : "");
      });
    } catch (const std::system_error& error) {
      REXLOG_WARN("[native] U1 shader preload: could not create the thread ({})", error.what());
    }
  }

  void StopPreloadShaders() {
    preload_stop_.store(true, std::memory_order_relaxed);
    if (preload_thread_.joinable()) {
      preload_thread_.join();  // at most one entry read
    }
  }

  // Ring only, on each submission: starts the thread once, as soon as the library is loaded.
  void TryPrewarm() {
    TryPreloadShaders();
    if (prewarmed_decided_) {
      return;
    }
    if (file_list_.empty() || cache_pipelines_ == VK_NULL_HANDLE ||
        !REXCVAR_GET(masseffect_native_pipelines_prewarm)) {
      prewarmed_decided_ = true;
      if (!file_list_.empty()) {
        REXLOG_INFO("[native] C6 prewarm: not prewarming ({})",
                    cache_pipelines_ == VK_NULL_HANDLE ? "no pipeline cache"
                                                       : "masseffect_native_pipelines_prewarm = false");
      }
      return;
    }
    const ShadersNative* library = ActiveLibrary();
    if (!library || !library->loaded() || layout_pipeline_ == VK_NULL_HANDLE) {
      return;  // not yet: check again on the next submission
    }
    prewarmed_decided_ = true;
    prewarmed_library_ = library;
    prewarmed_eds_ = eds_mode_;  // the ring would not request those of another dynamic state mode
    prewarmed_start_ = std::chrono::steady_clock::now();
    try {
      prewarmed_thread_ = std::thread([this] { PrewarmedLoop(); });
      REXLOG_INFO("[native] C6 prewarm: thread created for {} pipelines from the list",
                  file_list_.size());
    } catch (const std::system_error& error) {
      REXLOG_WARN("[native] C6 prewarm: could not create the thread ({}); not prewarming", error.what());
    }
  }

  // The thread. It only reads file_list_, the library, the layout and the cache; it writes
  // state_list_[i] before publishing prewarmed_until_ = i + 1, and its atomic counters. Its modules
  // and render pass are its own and it destroys them when done.
  void PrewarmedLoop() {
    rex::thread::set_current_thread_name("MASSEFFECT pipeline prewarm");
    int32_t priority = -1;
    // The lowest priority the system accepts, and never above the guest's (0x3B): compiling at the priority
    // threads are born with would take the core from the game and the ring. If none is accepted, nothing is
    // compiled.
    for (const int32_t candidate : {0x3F, 0x3E, 0x3D, 0x3C, 0x3B}) {
      if (RexSwitchSetCurrentThreadPriorityOk(int(candidate))) {
        priority = candidate;
        break;
      }
    }
    if (priority < 0) {
      REXLOG_WARN("[native] C6 prewarm: the system accepts no priority from 0x3B to 0x3F: not "
                  "prewarming");
      prewarmed_finished_.store(true, std::memory_order_release);
      return;
    }
    prewarmed_priority_.store(priority, std::memory_order_relaxed);
    const ShadersNative& library = *prewarmed_library_;
    std::unordered_map<uint64_t, VkShaderModule> modules;  // (variant << 32) | number
    std::unordered_map<uint64_t, VkRenderPass> passes;       // by formats
    const auto create = [&](const uint32_t* spirv, size_t bytes, bool track = true) {
      VkShaderModuleCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = bytes;
      info.pCode = spirv;
      VkShaderModule shader_module = VK_NULL_HANDLE;
      if (dfn_.vkCreateShaderModule(device_, &info, nullptr, &shader_module) != VK_SUCCESS) {
        shader_module = VK_NULL_HANDLE;
      }
      if (track) RegistrarModuleCode(shader_module, std::vector<uint32_t>(spirv, spirv + bytes / 4));
      return shader_module;
    };
    // ModuleFor: the library's SPIR-V as is.
    const auto normal = [&](const ShaderEntry& e) {
      auto it = modules.find(e.number);
      if (it == modules.end()) {
        it = modules.emplace(e.number, create(e.shader->Spirv().data(), e.shader->Spirv().size() * sizeof(uint32_t), false)).first;
        if (!e.vertices) RegistrarModuleCode(it->second, e.shader->Spirv(), true);
      }
      return it->second;
    };
    // The pixel shader PipelineFor would choose, in the same order: the 7e3
    // output-clamped variant, without color writes and with early tests. The transformed variants use the
    // same fallback as the ring: if they cannot be patched, use the normal module.
    const auto pixel_no_half = [&](const PipelineKey& key, const ShaderEntry& e) {
      const uint32_t spec = key.specialization;
      const uint32_t variant = (spec & kSpecTarget7e3)      ? 5
                                : (spec & kSpecAlphaOnly)        ? 3
                                : (spec & kSpecZEarly)       ? 4
                                                                : 0;
      if (variant == 0) {
        return normal(e);
      }
      const uint32_t mask_7e3 =
          (spec & kSpecMask7e3) >> kSpecMask7e3Displacement;
      const uint32_t label_variant = variant | (variant == 5 ? mask_7e3 << 8 : 0);
      const uint64_t shader_module_key =
          (uint64_t(label_variant) << 32) | uint64_t(e.number);
      auto it = modules.find(shader_module_key);
      if (it == modules.end()) {
        VkShaderModule shader_module = VK_NULL_HANDLE;
        if (variant == 3) {
          uint32_t removed = 0;
          const std::vector<uint32_t> pruned = PruneWritesOfColor(e.shader->Spirv(), removed);
          if (!pruned.empty()) {
            shader_module = create(pruned.data(), pruned.size() * sizeof(uint32_t));
          }
        } else if (variant == 5) {
          const std::vector<uint32_t> limited = LimitOutputs7e3(e.shader->Spirv(), mask_7e3);
          if (!limited.empty()) {
            shader_module = create(limited.data(), limited.size() * sizeof(uint32_t));
          }
        } else {
          const char* reason = "";
          const std::vector<uint32_t> patched = WithEarlyTests(e.shader->Spirv(), reason);
          if (!patched.empty()) {
            shader_module = create(patched.data(), patched.size() * sizeof(uint32_t));
          }
        }
        it = modules.emplace(shader_module_key, shader_module).first;
      }
      if (it->second != VK_NULL_HANDLE) {
        return it->second;
      }
      return normal(e);
    };
    const auto pixel = [&](const PipelineKey& key, const ShaderEntry& e) {
      auto selected = pixel_no_half(key, e);
      if ((key.specialization & kSpecRasterGridX) ||
          (key.rasterization & me::native::kNativeMsaa2PhaseProbe)) {
        selected = ModuleGuestFragCoordXY(selected, key, nullptr, &e);
        if (!selected) return VkShaderModule(VK_NULL_HANDLE);
      }
      if (key.specialization & kSpecDepthFloat24Quantize)
        return ModuleDepthQuantize(selected, bool(key.specialization & kSpecDepthFloat24Round));
      return (key.specialization & kSpecDepthFloat24Half) ? ModuleDepthHalf(selected) : selected;
    };
    uint32_t done = 0, compiled = 0, no_shader = 0, other_mode = 0, failed = 0;
    uint64_t ns_compiled = 0;
    const size_t n = file_list_.size();
    for (size_t i = 0; i < n; ++i) {
      if (prewarmed_stop_.load(std::memory_order_relaxed)) {
        break;
      }
      const RegisterPipeline& r = file_list_[i];
      uint8_t state = kFailedList;
      // Package order changes whenever discovery adds a shader. Prefer the stable container fingerprint;
      // the numeric key is retained only for compatibility with records written before fingerprints existed.
      const ShaderEntry* vs = r.vs_fingerprint ? library.PerFingerprint(r.vs_fingerprint)
                                            : (r.key.vs ? library.PerNumber(r.key.vs - 1) : nullptr);
      const ShaderEntry* ps = r.ps_fingerprint ? library.PerFingerprint(r.ps_fingerprint)
                                            : (r.key.ps ? library.PerNumber(r.key.ps - 1) : nullptr);
      if (!vs || !vs->vertices || !vs->shader || vs->shader->fingerprint != r.vs_fingerprint ||
          (r.key.ps && (!ps || ps->vertices || !ps->shader || ps->shader->fingerprint != r.ps_fingerprint))) {
        state = kListNoShader;
        ++no_shader;
      } else if (r.key.fill2 != prewarmed_eds_) {
        state = kOtherListMode;
        ++other_mode;
      } else {
        VkShaderModule module_vs = VK_NULL_HANDLE;
        if (r.key.specialization & kSpecRectangle) {
          const uint64_t module_key = (uint64_t(6) << 32) | vs->number;
          auto it = modules.find(module_key);
          if (it == modules.end()) {
            const auto rectangle = me::native::ExpandRectangleShader(vs->shader->Spirv());
            const auto module = rectangle.words.empty() ? VK_NULL_HANDLE
                : create(rectangle.words.data(), rectangle.words.size()*sizeof(uint32_t), false);
            it = modules.emplace(module_key, module).first;
          }
          module_vs = it->second;
        } else module_vs = normal(*vs);
        const VkShaderModule module_ps = r.key.ps ? pixel(r.key, *ps) :
            (r.key.specialization & kSpecDepthFloat24Quantize) ?
                ModuleDepthQuantize(VK_NULL_HANDLE, bool(r.key.specialization & kSpecDepthFloat24Round), true) : VK_NULL_HANDLE;
        const uint64_t pass_key = XXH3_64bits(r.key.formats, sizeof(r.key.formats));
        auto it_pass = passes.find(pass_key);
        if (it_pass == passes.end()) {
          it_pass = passes.emplace(pass_key, CreatePass(r.key.formats, kLoadRead)).first;
        }
        VerticesEntry entry;
        for (uint32_t k = 0; k < r.n_attributes; ++k) {
          const AttributeRegister& a = r.attributes[k];
          entry.attributes.push_back({a.location, a.binding, VkFormat(a.format), a.offset});
        }
        for (uint32_t k = 0; k < r.n_bindings; ++k) {
          entry.bindings.push_back({0, r.strides[k]});
        }
        if (module_vs != VK_NULL_HANDLE &&
            (!(r.key.ps || (r.key.specialization & kSpecDepthFloat24Quantize)) || module_ps != VK_NULL_HANDLE) &&
            it_pass->second != VK_NULL_HANDLE) {
          VkPipeline pipeline = VK_NULL_HANDLE;
          uint32_t n_colors = 0;
          const auto t0 = std::chrono::steady_clock::now();
          const VkResult result =
              CreatePipelineVulkan(r.key, entry, module_vs, module_ps, it_pass->second, false, pipeline, n_colors);
          const uint64_t ns = uint64_t(
              std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
          if (result == VK_SUCCESS && pipeline != VK_NULL_HANDLE) {
            dfn_.vkDestroyPipeline(device_, pipeline, nullptr);
            state = kPrewarmedList;
            ++done;
            if (ns >= kNsCompiled) {
              ++compiled;
              ns_compiled += ns;
            }
          } else {
            ++failed;
          }
        } else {
          ++failed;
        }
      }
      state_list_[i] = state;
      prewarmed_done_.store(done, std::memory_order_relaxed);
      prewarmed_compiled_.store(compiled, std::memory_order_relaxed);
      prewarmed_ns_compiled_.store(ns_compiled, std::memory_order_relaxed);
      prewarmed_no_shader_.store(no_shader, std::memory_order_relaxed);
      prewarmed_other_mode_.store(other_mode, std::memory_order_relaxed);
      prewarmed_failed_.store(failed, std::memory_order_relaxed);
      prewarmed_until_.store(i + 1, std::memory_order_release);
    }
    for (const auto& [key, shader_module] : modules) {
      if (shader_module != VK_NULL_HANDLE) {
        ForgetModuleCode(shader_module);
        dfn_.vkDestroyShaderModule(device_, shader_module, nullptr);
      }
    }
    for (const auto& [key, pass] : passes) {
      if (pass != VK_NULL_HANDLE) {
        dfn_.vkDestroyRenderPass(device_, pass, nullptr);
      }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - prewarmed_start_).count();
    REXLOG_INFO("[native] C6 prewarm: {} in {:.1f} s, priority {:#x}: {} of {} pipelines "
                "prewarmed, {} really compiled ({:.0f} ms, {:.1f} ms each; the rest was already in the cache), "
                "{} skipped because the library no longer has their shaders, {} from another dynamic-state mode and {} "
                "failed",
                prewarmed_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished", seconds, priority,
                done, n, compiled, double(ns_compiled) / 1e6,
                compiled ? double(ns_compiled) / 1e6 / double(compiled) : 0.0, no_shader, other_mode, failed);
    prewarmed_finished_.store(true, std::memory_order_release);
  }

  // Ring only, every 10 s if anything changed: the thread's progress, what the ring had to create, and
  // the guard (see above).
  void PrewarmedReport(std::chrono::steady_clock::time_point now) {
    if (now - prewarmed_report_ < std::chrono::seconds(10)) {
      return;
    }
    prewarmed_report_ = now;
    if (!prewarmed_difference_ && prewarmed_ring_ >= 8 &&
        prewarmed_slow_ring_ * 2 >= prewarmed_ring_) {
      prewarmed_difference_ = true;
      prewarmed_stop_.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("[native] C6 prewarm: MISMATCH: of {} pipelines the ring requested that were already "
                   "prewarmed, {} took as long as a compiled one (20 ms or more): what the thread prewarms is not what "
                   "the ring requests. The thread is stopped; nothing else changes",
                   prewarmed_ring_, prewarmed_slow_ring_);
    }
    const size_t until = prewarmed_until_.load(std::memory_order_acquire);
    const bool finished = prewarmed_finished_.load(std::memory_order_acquire);
    const uint64_t changes = uint64_t(until) + prewarmed_ring_ + ring_of_list_ + new_ring_ +
                             (finished ? 1 : 0) + (prewarmed_decided_ ? 1 : 0);
    if (changes == prewarmed_previous_changes_) {
      return;
    }
    prewarmed_previous_changes_ = changes;
    const auto media = [](uint64_t ns, uint64_t k) { return k ? double(ns) / double(k) / 1e6 : 0.0; };
    const uint32_t compiled = prewarmed_compiled_.load(std::memory_order_relaxed);
    MASSEFFECT_REPORT_RING(
        "[native] C6 pipeline prewarm: list of {}; thread {} (priority {:#x}): {} visited, {} "
        "prewarmed, {} really compiled ({:.0f} ms), {} without their shaders, {} of another mode, {} failed | the "
        "ring created {} from the list already prewarmed ({:.1f} ms average, {} slow), {} from the list not prewarmed "
        "({:.1f} ms average) and {} new ({:.1f} ms average; {} added to the list)",
        file_list_.size(),
        !prewarmed_thread_.joinable() ? (prewarmed_decided_ ? "not launched" : "waiting for the library")
        : finished                    ? (prewarmed_stop_.load(std::memory_order_relaxed) ? "stopped" : "finished")
                                       : "running",
        uint32_t(prewarmed_priority_.load(std::memory_order_relaxed)), until,
        prewarmed_done_.load(std::memory_order_relaxed), compiled,
        double(prewarmed_ns_compiled_.load(std::memory_order_relaxed)) / 1e6,
        prewarmed_no_shader_.load(std::memory_order_relaxed), prewarmed_other_mode_.load(std::memory_order_relaxed),
        prewarmed_failed_.load(std::memory_order_relaxed), prewarmed_ring_,
        media(ns_prewarmed_ring_, prewarmed_ring_), prewarmed_slow_ring_, ring_of_list_,
        media(ns_ring_of_list_, ring_of_list_), new_ring_, media(ns_new_ring_, new_ring_),
        ring_to_list_);
  }

  void StopPrewarmed() {
    prewarmed_stop_.store(true, std::memory_order_relaxed);
    if (prewarmed_thread_.joinable()) {
      prewarmed_thread_.join();  // at most, as long as the pipeline being compiled takes
    }
  }

  void DestroyImage(ImageNative& image) {
    if (image.view != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, image.image, nullptr);
    // The pool chunk is returned after destroying the image that used it, and in that case `memory` is
    // NULL on purpose: the memory belongs to a shared slab and is not freed on its own.
    if (image.pool_block != 0xFFFFFFFFu) pool_textures_.Release(image.pool_block);
    if (image.memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, image.memory, nullptr);
    image = ImageNative{};
  }

  struct Heap {
    uint32_t capacity = 0;
    uint32_t next = 1;
    std::vector<uint32_t> free;
  };

  const VulkanDevice* vulkan_device_;
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* memory_;
  ContextTargets* context_;
  FnBufferAddress buffer_address_ = nullptr;
  PFN_vkCmdInsertDebugUtilsLabelEXT label_gpu_ = nullptr;

  VkBuffer upload_ = VK_NULL_HANDLE;
  VkDeviceMemory upload_memory_ = VK_NULL_HANDLE;
  uint32_t upload_type_ = UINT32_MAX;
  VkDeviceSize upload_real_size_ = 0;
  uint8_t* upload_data_ = nullptr;
  VkDeviceAddress upload_address_ = 0;
  VkDeviceSize used_upload_ = 0;
  bool coherent_upload_ = false;
  uint64_t epoch_upload_ = 0;
  uint64_t frame_ = 0;
  // Fast untiling and its guard (see ReadLevel).
  static constexpr uint32_t kLevelsToCheck = 200;  // it used to be 2000
  static constexpr uint32_t kCheckAOfEach = 64;   // after the first ones, by sampling
  uint64_t seen_levels_ = 0;
  uint64_t seen_orders_ = 0;
  int tile_fast_ = -1;
  uint32_t checked_levels_ = 0;
  uint32_t checked_orders_ = 0;
  std::vector<uint8_t> check_tile_;
  // Texture check budget (see PrepareTexture).
  static constexpr uint32_t kPostponementMax = 8;
  int32_t budget_fingerprints_ = -1;
  uint64_t frame_fingerprints_ = UINT64_MAX;
  uint64_t bytes_frame_fingerprint_ = 0;
  // Sampled recheck of stable textures (see PrepareTexture and SampleFingerprint).
  static constexpr uint64_t kSamplesToCheck = 3000;  // the first ones, with both hashes: the guard
  int32_t sampling_fingerprints_ = -1;      // -1 cvar not read; 0 off; N: 1 in N rechecks is full
  uint64_t checked_samples_ = 0;  // stable rechecks with both hashes, since start-up
  uint64_t samples_with_the_two_ = 0;  // the same, since the last report line
  uint64_t samples_hits_ = 0;     // decided on the sample alone
  uint64_t distinct_samples_ = 0;    // the sample changed and the full path followed
  uint64_t bytes_sample_ = 0;
  uint64_t bytes_saved_sample_ = 0;
  std::chrono::steady_clock::time_point report_sampling_{};
  // Vertex copies on a separate thread (EnqueueCopy). Power-of-2 capacity.
  const bool active_copies_ = REXCVAR_GET(masseffect_native_uploads_thread);
  std::array<WorkCopy, 8192> copies_{};
  std::atomic<size_t> written_copies_{0};
  // How many copies are done, whether by the thread or the ring (see WaitUploads). Outside
  // WaitUploads they finish in order, and then it is also the index of the first one not done.
  std::atomic<size_t> done_copies_{0};
  bool copies_sleeping_ = false;   // under copies_mutex_: the copy thread sleeps or is about to
  bool copies_waiting_ = false;   // under copies_mutex_: the ring waits in WaitUploads
  std::mutex copies_mutex_;
  std::condition_variable copies_cv_;
  std::condition_variable done_copies_cv_;
  bool copies_stop_ = false;  // guarded by copies_mutex_
  std::thread copies_thread_;
  uint64_t copies_in_line_ = 0;
  uint64_t waits_copies_ = 0;
  uint64_t ns_waiting_copies_ = 0;
  std::atomic<size_t> copies_progress_{0};  // copies done by the thread, one by one (diagnostic)
  std::thread::id copies_producer_;
  bool copies_other_warned_thread_ = false;
  // The ring's help (masseffect_native_uploads_help) and its guard. See WaitUploads.
  const bool copies_requested_help_ = REXCVAR_GET(masseffect_native_uploads_help);
  int32_t copies_phase_ = 0;         // PhaseCopies; ring only
  bool copies_no_thread_ = false;    // off after a MISMATCH: copies go on the ring; ring only
  std::atomic<size_t> taken_copies_{0};  // how many were taken, in order, by the thread or the ring (with CAS)
  std::mutex copies_chunk_mutex_;          // held by the thread while it copies what it took (lends priority)
  std::array<std::atomic<uint32_t>, 8192> copies_marks_{};  // index + 1 of the last copy done in each slot
  std::atomic<uint64_t> copies_thread_n_{0};  // copies done by the thread, for the report
  std::atomic<uint64_t> copies_thread_bytes_{0};
  size_t verified_copies_ = 0;  // marks checked up to here; ring only
  uint64_t checked_copies_ = 0;
  uint64_t copies_waits_looking_ = 0;
  uint64_t helped_copies_ = 0;
  uint64_t copies_bytes_helped_ = 0;
  uint64_t copies_ns_helping_ = 0;
  uint64_t copies_differences_ = 0;
  struct ReportCopiesFigures {  // the last 10 s (ReportCopies); ring only
    uint64_t queued = 0, bytes_queued = 0, in_line = 0, bytes_in_line = 0;
    uint64_t helped = 0, bytes_helped = 0, ns_helping = 0, waits_with_help = 0;
    uint64_t waits = 0, no_take = 0, waits_chunk = 0, ns_chunk = 0, ns_chunk_worst = 0;
    uint64_t whole_waits = 0, ns_wait = 0, ns_wait_worst = 0, checked = 0;
  };
  ReportCopiesFigures copies_inf_{};
  std::chrono::steady_clock::time_point copies_report_{};
  uint64_t copies_thread_n_previous_ = 0;
  uint64_t copies_thread_bytes_previous_ = 0;
  struct BufferUpload {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize real_size = 0;
    uint8_t* data = nullptr;
    VkDeviceAddress address = 0;
  };
  // One per work slot (there are 3). With fewer than there are slots, two slots would share a buffer and
  // the CPU would write over what the GPU is still reading.
  std::array<BufferUpload, kSlotsOfWork> uploads_{};  // upload_* are the current slot's
  // Separate shared constants, CPU-cached (masseffect_shared_native_cache).
  std::array<BufferUpload, kSlotsOfWork> shared_bufs_{};
  bool shared_separate_ = false;
  bool shared_coherent_ = true;
  uint32_t shared_type_ = 0;
  uint8_t* shared_data_ = nullptr;
  VkDeviceMemory shared_memory_ = VK_NULL_HANDLE;
  VkDeviceSize shared_real_size_ = 0;
  VkDeviceSize shared_used_ = 0;

  std::array<VkDescriptorSetLayout, 4> layouts_{};
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 4> sets_{};
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;
  // Set 4, the constants through dynamic UBOs. One set per upload slot (each one is a VkBuffer).
  bool use_ubo_ = false;
  VkDeviceSize alignment_ubo_ = 256;
  VkDescriptorSetLayout layout_ubo_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ubo_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlotsOfWork> sets_ubo_{};  // one per work slot
  uint32_t slot_current_ = 0;
  bool ubo_bound_ = false;
  uint32_t slot_ubo_bound_ = UINT32_MAX;
  std::array<uint32_t, 3> offsets_ubo_bound_{};
  // Set 4 by differences (ControlSet4, ReportSet4).
  std::array<uint64_t, 8> set4_changes_{};  // binds by what changes: bit 0 VS, bit 1 PS, bit 2 shared
  uint64_t set4_first_ = 0;              // binds after a new command buffer, a new slot or the sky
  uint64_t set4_draws_ = 0;
  std::array<uint64_t, 3> reuploaded_vs_{};  // constants uploaded again: by generation, by epoch, because they grow
  std::array<uint64_t, 3> reuploaded_ps_{};
  std::chrono::steady_clock::time_point set4_report_{};
  bool set4_request_ = true;
  bool set4_recorded_request_ = false;
  bool set4_warned_difference_ = false;
  uint64_t set4_nvk_previous_[9] = {};  // NVK counts at the previous report
  // The draw path in NVK (ControlDrawNvk and ReportDrawNvk). Ring only.
  bool nvk_preload_app_ = false;  // hand the pipeline to NVK after PipelineFor
  bool nvk_recorded_draw_ = false;
  bool nvk_off_warned_[5] = {};
  std::chrono::steady_clock::time_point nvk_report_{};
  uint64_t nvk_previous_parts_[16][2] = {};
  uint64_t nvk_previous_counts_[13] = {};
  uint64_t nvk_previous_improvements_[5][4] = {};
  uint64_t sends_ = 0;
  uint64_t sends_ubo_ = 0;
  std::array<Heap, 4> heaps_{};
  std::array<ImageNative, 3> empty_{};
  VkSampler sampler_empty_ = VK_NULL_HANDLE;
  bool empty_prepared_ = false;

  std::unordered_map<const ShaderEntry*, VkShaderModule> modules_;
  struct ModuleCodeDepth {
    const std::vector<uint32_t>* borrowed = nullptr;
    std::vector<uint32_t> owned;
  };
  struct ModuleDepthHalfEntry {
    std::vector<uint32_t> original;
    VkShaderModule module = VK_NULL_HANDLE;
  };
  struct ModuleDepthQuantizeEntry {
    std::vector<uint32_t> original;
    bool rounded = false;
    VkShaderModule module = VK_NULL_HANDLE;
  };
  struct ModuleFragCoordXYEntry {
    std::vector<uint32_t> original;
    me::native::GuestFragCoordXYMode mode;
    VkShaderModule module = VK_NULL_HANDLE;
  };
  std::mutex modules_depth_mutex_;
  std::unordered_map<VkShaderModule, ModuleCodeDepth> codes_modules_depth_;
  std::unordered_map<uint64_t, std::vector<ModuleDepthHalfEntry>> modules_depth_half_;
  std::unordered_map<uint64_t, std::vector<ModuleDepthQuantizeEntry>> modules_depth_quantize_;
  std::unordered_map<uint64_t, std::vector<ModuleFragCoordXYEntry>> modules_fragcoord_xy_;
  std::unordered_map<const ShaderEntry*, me::native::RectangleShader> shaders_rectangle_;
  std::unordered_map<const ShaderEntry*, VkShaderModule> modules_rectangle_;
  std::unordered_set<uint32_t> recorded_rectangles_;
  std::unordered_set<uint32_t> rectangles_recorded_constants_;
  uint64_t me_bound_collisions_ = 0;
  uint32_t me_depth_rectangle_audits_ = 0;
  std::unordered_map<uint64_t, std::pair<PipelineKey, VkPipeline>> pipelines_;
  // One-entry shortcut for PipelineFor (see the comment there).
  PipelineKey last_key_{};
  VkPipeline last_pipeline_ = VK_NULL_HANDLE;
  bool last_valid_key_ = false;
  // direct cache in front of pipelines_ (masseffect_native_pipelines_direct).
  struct CellPipeline {
    PipelineKey key{};
    VkPipeline pipeline = VK_NULL_HANDLE;  // VK_NULL_HANDLE = empty slot
  };
  static constexpr size_t kCellsPipeline = 256;  // power of 2; 256 x 88 bytes = 22 KB contiguous
  std::array<CellPipeline, kCellsPipeline> cells_pipeline_{};
  bool pipelines_direct_ = true;           // the cvar, once per frame
  bool pipelines_direct_off_ = false;  // the guard saw a difference
  uint64_t pipelines_direct_hits_ = 0;
  uint64_t pipelines_direct_failures_ = 0;
  uint64_t pipelines_direct_checked_ = 0;
  uint64_t pipelines_direct_previous_hits_ = 0;
  uint64_t pipelines_direct_previous_failures_ = 0;
  std::chrono::steady_clock::time_point pipelines_direct_report_{};
  static constexpr uint64_t kPipelinesToCheck = 200000;
  // masseffect_native_count_changes_pipeline (CountChangePipeline and ReportChangesPipeline).
  enum : uint32_t {
    kChangeBindings = 0,        // vkCmdBindPipeline calls from Draw, counted
    kChangeAfterPass,           // of those, the first of a pass (BeginPass forgets the bound pipeline)
    kFirstChange,            // no previous key in the buffer (new buffer or after the deferred sky)
    kIdenticalChange,           // the same key: only after BeginPass
    kChangeShaders,            // another VS or PS
    kChangeEntry,            // same shaders, different vertex input
    kChangeFormats,           // same shaders and input, different formats (another pass)
    kChangeSpecAlpha,           // all the above equal; specialization only changes the alpha test (bits 1, 16-18)
    kChangeSpecZEarly,      // ... the alpha test and the early Z (bit 20)
    kChangeSpecOther,           // ... other specialization bits
    kChangeNoEffect,          // state only, in bits PipelineFor does not read: effectively the same pipeline
    kChangeNoEffectAfterPass,  // ... and the first of a pass
    kChangeOnlyState,         // real state-only changes: what dynamic state would avoid
    kChangeAfterStatePass,     // ... and the first of a pass
    kChangeEds12,              // ... without blending, masks or another topology class (EDS1/EDS2, core 1.3)
    kChangeEds12AfterPass,      // ... and the first of a pass
    kFieldTopology,           // per field, on the canonical state (one bind may change several)
    kFieldClassTopology,
    kFieldBlend,
    kFieldMasks,
    kFieldZ,
    kFieldStencil,
    kFieldFace,
    kFieldReset,
    kFieldBias,
    kChangesN
  };
  std::array<uint64_t, kChangesN> changes_pipeline_{};
  PipelineKey bound_key_{};       // the last key bound in this command buffer
  bool bound_valid_key_ = false;   // false: new buffer, after the deferred sky, or not counting
  bool count_changes_pipeline_ = true;  // the cvar, once per frame (ReportChangesPipeline)
  uint64_t changes_previous_draws_ = 0;
  uint64_t changes_previous_frames_ = 0;
  std::chrono::steady_clock::time_point changes_report_{};
  // Dynamic state, phase 0a (masseffect_native_canonical_key, SearchKey).
  static constexpr uint64_t kCanonicalToCheck = 200000;
  bool canonical_key_ = true;           // the cvar, once per command buffer (UseSlot)
  bool canonical_off_key_ = false;  // the guard saw a difference
  bool canonical_valid_ = false;         // canonical_raw_ -> canonical_result_ is the previous draw's
  PipelineKey canonical_raw_{};
  PipelineKey canonical_result_{};
  uint64_t canonical_changed_ = 0;      // keys Canonicalize changes (not counting consecutive repeats)
  uint64_t canonical_checked_ = 0;
  uint64_t canonical_changed_previous_ = 0;
  uint64_t canonical_pipelines_previous_ = 0;
  std::chrono::steady_clock::time_point canonical_report_{};
  // Dynamic state, phase 0b (masseffect_native_pipeline_between_passes).
  bool pipeline_between_passes_ = true;  // the cvar, once per command buffer (UseSlot)
  bool pipeline_between_recorded_passes_ = false;
  uint64_t passes_with_pipeline_ = 0;   // passes started with a bound pipeline that is kept
  uint64_t between_passes_previous_passes_ = 0;
  uint64_t between_kept_previous_passes_ = 0;
  std::chrono::steady_clock::time_point between_passes_report_{};
  // Dynamic state phases 1 and 2 (PinDynamicState, its guard and its report).
  enum : uint32_t { kEds12 = 1, kEds3 = 2 };  // bits of eds_mode_ and of PipelineKey::fill2
  static constexpr VkDynamicState kDynamicEds12[10] = {
      VK_DYNAMIC_STATE_CULL_MODE,           VK_DYNAMIC_STATE_FRONT_FACE,
      VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,  VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,   VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
      VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
      VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
  static constexpr uint64_t kEdsToCheck = 200000;
  enum : uint32_t {
    kEdsFace = 0,
    kEdsFront,
    kEdsTopology,
    kEdsReset,
    kEdsBias,
    kEdsTestZ,
    kEdsWritesZ,
    kEdsFunctionZ,
    kEdsStencil,
    kEdsStencilOps,
    kEdsActiveBlend,  // phase 2
    kEdsEquation,      // phase 2
    kEdsMask,       // phase 2
    kEdsAll,          // times everything is set (not a call)
    kEdsN
  };
  PFN_vkCmdSetCullMode set_face_ = nullptr;  // Vulkan 1.3 core (LoadDynamicState)
  PFN_vkCmdSetFrontFace set_front_ = nullptr;
  PFN_vkCmdSetPrimitiveTopology set_topology_ = nullptr;
  PFN_vkCmdSetPrimitiveRestartEnable set_reset_ = nullptr;
  PFN_vkCmdSetDepthBiasEnable set_bias_ = nullptr;
  PFN_vkCmdSetDepthTestEnable set_test_z_ = nullptr;
  PFN_vkCmdSetDepthWriteEnable set_writes_z_ = nullptr;
  PFN_vkCmdSetDepthCompareOp set_function_z_ = nullptr;
  PFN_vkCmdSetStencilTestEnable set_stencil_ = nullptr;
  PFN_vkCmdSetStencilOp set_stencil_ops_ = nullptr;
  bool eds12_available_ = false;  // API 1.3 and the ten functions
  bool eds_off_ = false;       // the guard saw a difference: for the rest of the session, everything in the pipeline
  bool eds_recorded_mode_ = false;
  uint32_t eds_mode_ = 0;          // kEds12 | kEds3 of this command buffer (UseSlot; the guard sets it to 0)
  bool eds_valid_ = false;        // eds_recorded_ is what is set in this buffer (false: new buffer, sky or pass)
  EdsState eds_recorded_{};
  PipelineKey eds_key_{};      // raw key of the last state set
  uint64_t eds_draws_ = 0;       // draws with dynamic state
  uint64_t eds_repeated_ = 0;     // ... with the same raw key as the previous one: nothing to set or check
  uint64_t eds_checkable_ = 0;  // ... with something to set (what the guard counts)
  uint64_t eds_checked_ = 0;
  uint64_t eds_previous_draws_ = 0;
  uint64_t eds_repeated_previous_ = 0;
  uint64_t eds_previous_frames_ = 0;
  std::array<uint64_t, kEdsN> eds_calls_{};    // vkCmdSet* recorded per state since the last report
  std::array<uint64_t, 4> pipelines_per_mode_{};  // pipelines created per mode (fill2) since start-up
  std::chrono::steady_clock::time_point eds_report_{};
  // Dynamic state, phase 2 (VK_EXT_extended_dynamic_state3).
  static constexpr VkDynamicState kDynamicEds3[3] = {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                                                       VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT};
  // The same mapping as kFactors in FillFixedState (fields 2, 3 and 17-31 give ZERO).
  static constexpr VkBlendFactor kFactorsBlend[32] = {
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  PFN_vkCmdSetColorBlendEnableEXT set_active_blend_ = nullptr;
  PFN_vkCmdSetColorBlendEquationEXT set_equation_ = nullptr;
  PFN_vkCmdSetColorWriteMaskEXT set_mask_ = nullptr;
  bool eds3_available_ = false;  // the SDK enables the extension and its three features; the driver provides the functions
  std::unordered_map<uint64_t, VkRenderPass> passes_;
  std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
  // masseffect_native_framebuffers_forget_views. The views of each cached framebuffer (by the same key), the
  // ones retired by the guard, and the counts for the warning.
  std::unordered_map<uint64_t, std::array<VkImageView, 5>> framebuffers_views_;
  std::vector<VkFramebuffer> fb_retired_;
  bool fb_retire_ = false;
  uint64_t fb_forgotten_views_ = 0;
  uint64_t fb_forgotten_ = 0;
  std::unordered_map<uint64_t, Texture> textures_;
  // Measurement only (masseffect_native_diag_reuse): textures with a hash, by content key (content key ->
  // texture key; see NoteContentTexture), and the counts for the 10 s report.
  std::unordered_multimap<uint64_t, uint64_t> live_per_content_;
  uint64_t distinct_contents_ = 0;
  int32_t diag_reuse_ = -1;  // -1 = cvar not read
  uint64_t reuse_new_ = 0;
  uint64_t reuse_match_ = 0;
  uint64_t reuse_cold_ = 0;
  uint64_t reuse_no_look_ = 0;
  uint64_t reuse_bytes_new_ = 0;
  uint64_t reuse_bytes_match_ = 0;
  uint64_t reuse_bytes_cold_ = 0;
  uint32_t reuse_details_ = 0;
  std::chrono::steady_clock::time_point report_reuse_{};
  std::unordered_map<uint64_t, View> views_;
  // The views_ keys of each image. InvalidateImages (every resolve and every copy) used to walk all of
  // views_: 2.7 % of the ring thread in play. Maintained together with views_ in SlotView
  // (insertion) and in RemoveView (every removal).
  std::unordered_map<VkImage, std::vector<uint64_t>> views_per_image_;
  std::unordered_map<uint32_t, std::pair<VkSampler, uint32_t>> samplers_;
  // Diagnostic: filter values already logged (SlotSampler).
  uint32_t aniso_seen_ = 0;
  bool aniso_recorded_ = false;  // masseffect_anisotropic_native, one line when the first one is created
  // masseffect_native_shadows_pending_bias, read once per frame.
  int32_t shadows_pending_bias_ = 0;
  bool shadows_recorded_bias_ = false;
  uint32_t mip_max_seen_ = 0;
  std::array<bool, 1024> seen_biases_{};
  std::vector<Texture*> textures_to_upload_;
  uint64_t me_ordered_updates_ = 0;
  std::vector<uint8_t> temporal_;
  std::vector<uint32_t, NoInitialize<uint32_t>> indices_;
  std::vector<uint32_t, NoInitialize<uint32_t>> converted_;
  std::vector<uint16_t, NoInitialize<uint16_t>> indices16_;  // fast path: 16 bits, not converted
  // masseffect_native_indices_cache: guest index range -> its copy in this upload buffer (direct-mapped).
  struct IndicesEntryCache {
    uint64_t key = 0, offset = 0, fingerprint = 0, frame = UINT64_MAX;
    uint32_t count = 0, vmin = 0, vmax = 0, generation = 0;
  };
  std::array<IndicesEntryCache, 4096> indices_cache_{};
  bool indices_cache_active_ = false;
  uint32_t indices_cache_generation_ = 0, indices_cache_sinc_view_ = 0;
  uint64_t indices_cache_hits_ = 0, indices_cache_bytes_saved_ = 0, indices_cache_lost_ = 0;
  // IndicesFrom16 (masseffect_native_indices_neon). Ring only.
  int32_t indices_neon_ = -1;  // -1 cvar not read, 0 plain loop, 1 NEON
  uint64_t indices_neon_draws_ = 0;
  uint64_t indices_neon_checked_ = 0;
  std::vector<uint16_t, NoInitialize<uint16_t>> indices_neon_test_;
  // Dynamic state already recorded in the current command buffer and pass.
  bool recorded_state_ = false;
  uint64_t push_recorded_[3] = {};
  VkViewport viewport_recorded_{};
  VkRect2D recorded_scissor_{};
  float recorded_blend_[4] = {};
  float recorded_bias_[2] = {};  // depth bias: constant and slope
  float warned_bias_[2] = {};
  uint32_t warnings_bias_ = 0;
  bool label_pass_ = false;
  std::unordered_set<uint64_t> me_fetch_ambig_diagnosed_;
  uint32_t stencil_recorded_[2] = {};  // RB_STENCILREFMASK for front and back
  bool stencil_recorded_valid_ = false;
  VkIndexType indices_type_recorded_ = VK_INDEX_TYPE_MAX_ENUM;
  // Shared constants of the previous draw and where they were uploaded.
  uint32_t shared_previous_[kSharedWords] = {};
  uint64_t shared_looked_ = 0;    // C6 report, to decide how to make the block cheaper
  uint64_t shared_changed_ = 0;
  VkDeviceSize shared_offset_ = 0;
  uint64_t shared_epoch_ = UINT64_MAX;

  VerticesEntry entry_;
  const VerticesEntry* entry_current_ = &entry_;  // entry_ or a cache element
  std::unordered_map<uint64_t, VerticesEntry> inputs_cache_;
  uint64_t inputs_cache_hits_ = 0;
  const ShaderEntry* vs_entry_ = nullptr;
  uint64_t generation_entry_ = UINT64_MAX;
  bool valid_entry_ = false;

  uint64_t constants_vs_generation_ = UINT64_MAX;
  bool me_constant_audit_hdr_ = false;
  bool me_payload_hdr_ = false;
  uint32_t me_payload_vs_ = 0;
  uint32_t me_resolve_fetch_vs_ = 0, me_resolve_fetch_sampler_ = 0;
  int me_resolve_fetch_ps_ = -1;
  std::unordered_set<uint64_t> me_resolve_fetch_contracts_;
  bool me_resolve_fetch_cap_logged_ = false;
  bool me_resolved_fetch_failed_this_draw_ = false;
  int me_payload_ps_ = -1;
  uint64_t me_payload_checked_[2] = {}, me_payload_bytes_[2] = {};
  uint64_t me_payload_changed_[2] = {}, me_payload_staged_[2] = {};
  uint64_t me_payload_skipped_[2] = {}, me_payload_logged_[2] = {};
  uint64_t me_payload_invalid_ = 0;
  uint64_t me_constant_audit_checked_[2] = {};
  uint64_t me_constant_audit_mismatches_[2] = {};
  uint64_t me_constant_audit_bytes_[2] = {};
  uint64_t constants_vs_epoch_ = UINT64_MAX;
  VkDeviceSize constants_vs_offset_ = 0;
  uint64_t constants_ps_generation_ = UINT64_MAX;
  uint64_t constants_ps_epoch_ = UINT64_MAX;
  uint8_t constants_ps_mode_ = UINT8_MAX;
  uint32_t constants_ps_tonemap_scale_bits_ = UINT32_MAX;
  VkDeviceSize constants_ps_offset_ = 0;
  uint32_t constants_vs_bytes_ = 0;
  uint32_t constants_ps_bytes_ = 0;

  bool active_pass_ = false;
  uint32_t category_pass_ = kGpuOther;
  bool sent_after_shadows_ = false;        // once per frame
  // Per-frame copies of the diagnostic cvars (see the comment at the Swap).
  bool diag_vertices_repeated_ = false;
  bool diag_stats_draw_ = false;
  std::array<VkBuffer, 16> buffers_vertices_{};
  // Height actually used by each render target, deduced from the scissor. It only grows.
  std::unordered_map<uint32_t, uint32_t> util_height_;
  bool area_util_ = true;
  // Small per-draw savings. See their cvars.
  bool framing_cache_ = true;             // masseffect_native_framing_cache, once per frame
  bool framing_cache_off_ = false;    // its guard saw a difference
  bool valid_framing_ = false;
  bool framing_depth_half_ = false;
  uint32_t framing_phase_probe_ = 0;
  uint64_t framing_generation_ = 0;
  uint64_t framing_pass_series_ = 0;
  uint64_t pass_series_ = 0;                // incremented on every BeginPass
  VkViewport framing_viewport_{};
  VkRect2D framing_scissor_{};
  float framing_ndc_[4] = {};
  uint32_t empty_framing_ = 0;
  uint64_t framing_hits_ = 0;         // since start-up (the guard)
  uint64_t checked_framing_ = 0;
  uint64_t framing_hits_report_ = 0;
  uint64_t framing_computations_report_ = 0;
  static constexpr uint64_t kFramingsToCheck = 200000;
  bool util_height_memo_active_ = true;      // masseffect_native_util_height_memo
  bool util_height_memo_off_ = false;
  uint32_t* util_height_memo_ = nullptr;     // util_height_ element of the last pitch (the map never erases)
  uint32_t util_height_memo_pitch_ = 0;
  uint64_t util_height_memo_hits_ = 0;
  uint64_t util_height_memo_previous_hits_ = 0;
  static constexpr uint64_t kUtilHeightToCheck = 200000;
  bool key_fast_pass_ = true;          // masseffect_native_key_fast_pass
  bool key_fast_off_pass_ = false;
  uint64_t pass_keys_[5] = {};           // the render target bytes that produced pass_key_
  bool pass_valid_keys_ = false;
  uint64_t keys_fast_pass_ = 0;
  uint64_t keys_fast_previous_pass_ = 0;
  static constexpr uint64_t kKeysPassToCheck = 200000;
  bool cvars_per_frame_ = true;        // masseffect_native_cvars_per_frame
  bool ps_alpha_only_frame_ = true;
  bool no_ps_no_color_frame_ = true;
  std::chrono::steady_clock::time_point minutiae_report_{};
  bool no_vegetation_ = false;
  // Shadow map vegetation draws discarded early (see the early discard in Draw).
  uint64_t draws_vegetation_soon_ = 0;
  /*
   * The sky draw, saved in full to replay later (masseffect_native_postponed_sky).
   *
   * This holds everything the vkCmd* calls of that draw need, by copy: handles (which do not change
   * within the pass) and values (which depend on no guest register once copied). The only thing kept "by
   * reference" are the offsets within the upload buffer, and that buffer only moves forward until
   * UseSlot resets it, always after the pass is closed.
   */
  enum : uint32_t {
    kSkyPerBlend = 0,  // a blended draw arrived: that one does read the background color
    kSkyPerNoZ,        // an opaque draw without Z write arrived: it could not cover the sky
    kSkyPerPassEnd,   // the pass closes (fallback path)
    kSkyPerOtherSky,   // a second sky in the same pass: the first one is emitted now
    kSkyReasons
  };
  struct PostponedSky {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;   // the upload one: vertices and indices
    bool usa_ubo = false;
    uint64_t push[3] = {};             // only with constants through pointers
    std::array<uint32_t, 3> offsets_ubo{};
    uint32_t slot_ubo = 0;
    VkViewport viewport{};
    VkRect2D scissor{};
    float blend[4] = {};
    float bias[2] = {};
    bool with_stencil = false;
    uint32_t stencil[2] = {};          // RB_STENCILREFMASK for front and back
    uint32_t n_bindings = 0;
    std::array<VkDeviceSize, 16> offsets_vertices{};
    bool with_indices = false;
    bool indices_from_16 = false;
    uint32_t indices = 0;
    uint32_t first_index = 0;
    uint32_t vmin = 0;
    uint32_t count = 0;               // no indices
    uint32_t ps_more_one = 0;           // for the diagnostic per-draw query
    uint32_t vs_more_one = 0;           // Mass Effect: the same, its vertex shader
    uint32_t category = 0;
    uint64_t draws_on_postpone = 0;   // position in the pass when it was deferred
    uint32_t eds_mode = 0;  // phases 1 and 2 its pipeline was looked up with (eds_mode_)
    EdsState eds{};        // and the state that pipeline lacks
  };
  PostponedSky sky_{};
  bool pending_sky_ = false;
  bool postponed_sky_ = true;  // the cvar, read once per frame
  bool recorded_sky_ = false;
  uint64_t draws_in_pass_ = 0;
  uint64_t seen_sky_ = 0;
  uint64_t seen_previous_sky_ = 0;
  // The ones that pass both marks (480 indices and first of the pass). This is the number to watch: if it
  // is not 1.00 per frame, the criterion still does not isolate the dome and nothing should be deferred.
  // The fingerprint-only "detected" once gave 7.00.
  uint64_t sky_candidates_ = 0;
  uint64_t sky_previous_candidates_ = 0;
  /*
   * The self-checking guard.
   *
   * Two earlier versions broke the image by enabling this blindly. The check that was missing ("the
   * criterion detects one draw per frame, not seven") no longer needs a diagnostic build: the game itself
   * runs it, before anything is deferred.
   *
   * Phase 0, observing: for the first kSkyFramesTest frames nothing is deferred (the image
   *   is exactly the non-deferred one) and the candidates in each frame are counted.
   * Phase 1, deferring: only if those frames never had more than one and at least kSkyFramesWithOne
   *   had exactly one. Then yes, and that is 3.4-4.3 ms of GPU time.
   * Phase 2, off for good: as soon as two are seen in the same frame, or if the test does not come out
   *   clean. It is not retried for the whole session.
   *
   * So the worst case of this change is that it does nothing. Breaking the image is not among the
   * possible outcomes.
   */
  enum : uint32_t { kSkyLooking = 0, kSkyPostponing = 1, kDiscardedSky = 2 };
  uint32_t sky_guard_ = kSkyLooking;
  uint32_t sky_guard_frames_ = 0;
  uint32_t sky_guard_with_one_ = 0;
  uint32_t sky_guard_in_frame_ = 0;
  uint32_t sky_guard_max_ = 0;
  uint64_t postponed_sky_count_ = 0;
  uint64_t postponed_previous_sky_ = 0;
  uint64_t sky_no_postponable_ = 0;
  uint64_t sky_two_in_pass_ = 0;
  uint64_t lost_sky_ = 0;
  std::array<uint64_t, kSkyReasons> emitted_sky_{};
  std::array<uint64_t, kSkyReasons> emitted_previous_sky_{};
  uint64_t sky_position_sum_ = 0;
  uint64_t sky_position_previous_sum_ = 0;
  uint64_t sky_position_max_ = 0;
  // The last binding made with vkCmdBindVertexBuffers, so it is not repeated.
  std::array<VkDeviceSize, 16> offsets_recorded_{};
  uint32_t recorded_bindings_ = 0;
  // masseffect_native_vertices_base_zero (see the cvar) and its guard.
  bool vertices_base_zero_ = true;           // the cvar, read once per frame (UseSlot)
  bool vertices_base_off_zero_ = false;  // the guard saw a difference: the rest of the session, as before
  uint64_t base_checked_zero_ = 0;       // draws on the base-zero path since start-up (the guard)
  uint64_t base_zero_draws_ = 0;           // since the last report: without binding their own offset
  uint64_t base_zero_total_ = 0;             // since the last report: draws recorded with vertices
  uint64_t base_zero_several_bindings_ = 0;
  uint64_t base_misaligned_zero_ = 0;      // one binding, but the dedupe gave a copy off a multiple
  uint64_t base_zero_recorded_bindings_ = 0;  // vkCmdBindVertexBuffers recorded since the last report
  std::chrono::steady_clock::time_point base_zero_report_{};
  static constexpr uint64_t kBaseZeroToCheck = 200000;
  std::array<uint64_t, 19> sub_ns_{};  // C6 substages, measurement only
  std::array<uint64_t, 19> sub_n_{};
  uint64_t sub_samples_ = 0;
  std::chrono::steady_clock::time_point sub_next_{};
  uint32_t stats_pass_ = UINT32_MAX;
  VkCommandBuffer pass_commands_ = VK_NULL_HANDLE;
  uint64_t pass_key_ = 0;
  uint64_t pass_generation_ = UINT64_MAX;
  uint32_t pass_width_ = 0;
  uint32_t pass_height_ = 0;
  float pass_scale_ = 1.0f;  // 1 except in the scaled shadow map
  float pass_raster_scale_x_ = 1.0f;  // independent host-only X grid expansion
  std::atomic<uint64_t> raster_grid_ps_rejected_{0};
  std::chrono::steady_clock::time_point pass_start_{};  // pass change breakdown
  uint32_t pass_formats_[5] = {};
  bool pass_depth_float24_half_ = false;
  VkRenderPass pass_rp_ = VK_NULL_HANDLE;
  uint64_t recording_generation_ = UINT64_MAX;
  VkPipeline pipeline_bound_ = VK_NULL_HANDLE;
  bool sets_bound_ = false;

  std::unordered_set<uint32_t> warned_vs_;
  uint32_t warnings_indices_ = 0;
  uint32_t warnings_swizzle_ = 0;
  uint64_t drawn_ = 0;
  uint64_t drawn_timed_ = 0;  // of those, with the stage stopwatch
  uint32_t stopwatch_counter_ = 0;
  bool time_ = false;  // the current draw carries the stage stopwatch
  uint64_t rejected_ = 0;
  // 8 stages. Stage 7 is the pass change, split from the pass stage to tell how much of the 3.9 us per
  // draw is the actual change (15 per frame at 162 us) and how much is what is done on every draw.
  std::array<uint64_t, kStagesDraw> stages_ns_{};
  std::unordered_map<uint32_t, uint64_t> causes_;
  uint32_t last_cause_ = 0;
  uint32_t cause_entry_ = 0;  // cause of the failed vertex input
  uint64_t uploads_texture_ = 0;
  uint64_t megabytes_bytes_ = 0;
  uint64_t bytes_textures_ = 0;  // texture images created (C6 report)
  int32_t textures_mb_max_ = 0;   // masseffect_native_textures_mb_max
  // Large slabs from which textures take chunks, instead of a dedicated allocation per texture (1.9 ms of
  // CPU each on Horizon). Finish() goes in the destructor after the DestroyImage loops, never before.
  PoolTextures pool_textures_;
  // Texture bind thread (masseffect_native_textures_binding_thread). See the LoopBindings block.
  struct SubmissionBinding {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkResult result = VK_NOT_READY;  // written by the thread, under bindings_mutex_
    uint64_t ns = 0;                    // wall time inside vkBindImageMemory, on the thread
  };
  struct TextureInFlight {  // ring thread only
    Texture* texture = nullptr;
    uint64_t ticket = 0;
    VkImage image = VK_NULL_HANDLE;  // the queued one (to remove it from images_in_flight_ and move its views)
    VkResult result = VK_NOT_READY;
    VkFormat format = VK_FORMAT_UNDEFINED;  // the fields below, to recreate it through CreateTexture if the bind fails
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layers = 1;
    uint32_t background = 0;
    uint32_t levels = 1;
    bool copy = false;  // UploadTexture already left its data in the upload buffer
    VkDeviceSize copy_offset = 0;
    uint64_t copy_epoch = 0;
    VkCommandBuffer copy_commands = VK_NULL_HANDLE;
  };
  struct ViewInFlight {  // ring thread only
    uint64_t key = 0;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t swizzle = 0;
    uint16_t swizzle_host = 0;
    uint32_t heap = 0;
    uint32_t slot = 0;
  };
  static constexpr uint64_t kQueueBindings = 256;  // power of 2
  static constexpr uint64_t kBindingsToCheck = 64;
  static constexpr int32_t kBindingsNoDecide = -1;
  static constexpr int32_t kOffBindings = 0;
  static constexpr int32_t kBindingsLooking = 1;
  static constexpr int32_t kBindingsApplying = 2;
  std::array<SubmissionBinding, kQueueBindings> queue_bindings_{};
  std::mutex bindings_mutex_;
  std::condition_variable bindings_cv_;         // there are requests or a stop request (the thread waits on it)
  std::condition_variable done_bindings_cv_;  // the thread finished one (the ring waits on it)
  uint64_t bindings_requests_ = 0;       // guarded by bindings_mutex_
  uint64_t done_bindings_ = 0;        // guarded by bindings_mutex_
  uint64_t collected_bindings_ = 0;     // guarded by bindings_mutex_
  bool bindings_sleeping_ = false;     // guarded by bindings_mutex_
  bool bindings_waiting_ = false;     // guarded by bindings_mutex_
  bool bindings_stop_ = false;         // guarded by bindings_mutex_
  bool bindings_priority_ok_ = true;   // guarded by bindings_mutex_
  uint64_t ns_bindings_thread_ = 0;       // guarded by bindings_mutex_
  uint64_t ns_binding_worst_ = 0;        // guarded by bindings_mutex_
  uint64_t bindings_full_queue_ = 0;    // guarded by bindings_mutex_
  std::thread bindings_thread_;
  int32_t bindings_priority_ = 0x2D;  // written by the ring before creating the thread
  // Ring thread only:
  int32_t bindings_phase_ = kBindingsNoDecide;
  bool stuck_bindings_ = false;
  bool measuring_creation_ = false;
  bool collecting_bindings_ = false;
  std::vector<TextureInFlight> in_flight_;
  std::vector<ViewInFlight> views_in_flight_;
  std::unordered_set<VkImage> images_in_flight_;
  uint64_t checked_bindings_ = 0;
  uint64_t bindings_thread_total_ = 0;
  uint64_t failed_bindings_ = 0;
  uint64_t postponed_views_ = 0;
  uint64_t postponed_copies_ = 0;
  uint64_t lost_slots_ = 0;
  uint64_t ns_wait_bindings_total_ = 0;
  // The 10 s report (ReportBindings):
  std::chrono::steady_clock::time_point report_bindings_{};
  uint64_t bindings_thread_report_ = 0;
  uint64_t waits_bindings_report_ = 0;
  uint64_t ns_wait_bindings_report_ = 0;
  uint64_t ns_wait_bindings_worst_ = 0;
  uint64_t ns_create_report_ = 0;
  uint64_t created_previous_report_ = 0;
  uint64_t postponed_views_report_ = 0;
  uint64_t postponed_copies_report_ = 0;
  uint64_t ns_bindings_previous_thread_ = 0;
  uint64_t full_previous_queue_ = 0;
  uint32_t reports_no_compensate_ = 0;
  bool releasing_per_missing_of_memory_ = false;  // guard against reentry
  uint64_t released_little_to_little_ = 0;
  uint64_t warning_trickle_ = 0;
  uint64_t attempt_eviction_ = 0;
  uint64_t released_textures_ = 0;
  // Diagnostic of why the cache grows: creations per address and last key per shape (address, format and
  // size). Only touched when a texture is created.
  uint64_t created_textures_ = 0;
  uint64_t created_in_view_address_ = 0;
  uint64_t created_same_shape_other_key_ = 0;
  std::unordered_map<uint32_t, uint32_t> created_per_address_;
  std::unordered_map<uint64_t, uint64_t> key_per_shape_;
  std::unordered_map<uint64_t, std::array<uint32_t, 5>> words_per_shape_;
  uint64_t incoherent_keys_ = 0;  // texture key guard (PrepareTexture)
  uint32_t warnings_other_key_ = 0;
  // "C6 counters".
  uint64_t started_passes_ = 0;
  uint64_t full_sends_ = 0;
  uint64_t ns_full_sends_ = 0;
  uint64_t bytes_vertices_ = 0;
  uint64_t bytes_indices_uploaded_ = 0;
  uint64_t samplers_prepared_ = 0;
  uint64_t samplers_cache_ = 0;
  uint64_t ns_passes_ = 0;
  uint64_t ns_vertices_ = 0;
  uint64_t computed_inputs_ = 0;
  uint64_t ns_inputs_ = 0;
  uint64_t reused_inputs_ = 0;
  uint64_t passes_per_generation_ = 0;
  uint64_t passes_per_target_ = 0;
  uint64_t resumed_passes_ = 0;
  uint64_t texels_passes_ = 0;
  std::array<uint64_t, kGpuCategories> texels_per_category_{};
  std::array<uint64_t, kGpuCategories> draws_per_category_{};
  bool inv_tex_size_ = false;
  bool pcf_cheap_ = false;                    // a single shadow map sample
  // Deduplication of vertex uploads within the frame.
  DedupeVertices dedupe_;
  bool dedupe_active_ = true;
  uint32_t dedupe_sinc_view_ = 0;  // last value seen of g_synchronizations_ring
  std::set<uint64_t> warnings_inv_resolved_size_;  // one trace per size
  std::unordered_map<const ShaderEntry*, VkShaderModule> modules_alpha_only_;
  // The same module with OpExecutionMode EarlyFragmentTests (masseffect_native_z_early).
  std::unordered_map<const ShaderEntry*, VkShaderModule> modules_z_early_;
  std::unordered_map<uint64_t, VkShaderModule> modules_7e3_;
  std::unordered_set<uint32_t> biases_color_recorded_;
  uint64_t draws_ps_useless_ = 0;     // no color and a PS that does not discard
  uint64_t draws_ps_required_ = 0;  // no color, but the PS is needed
  uint64_t shadows_active_alpha_ = 0;
  uint64_t scene_with_discard_ = 0;
  uint64_t scene_no_discard_ = 0;   // shadow map draws with the alpha test enabled
  /*
   * masseffect_native_z_early and masseffect_native_skip_invisibles. The first four split C6's "prevented by"
   * in two: those fixed by testing earlier and those that cannot be fixed because they write depth or
   * stencil (the latter are the exact size of a depth pre-pass).
   */
  enum : uint32_t {
    kZSet = 0,       // its depth test is moved earlier
    kZWritesZ,         // not possible: the draw writes depth
    kZStencil,         // not possible: stencil is involved
    kZAlreadyEarly,       // no alpha test or kill: the GPU already tested early
    kInvisibleBlend,   // blending copies the destination: the draw paints nothing
    kInvisibleAlpha,     // alpha test with the NEVER function
    kInvisibleColorOnly,  // invisible in color but writes Z: only its color is removed
    kZCounts
  };
  std::array<uint64_t, kZCounts> counts_z_{};
  std::array<uint64_t, kZCounts> counts_z_previous_{};
  uint64_t z_early_no_module_ = 0;
  uint64_t frames_z_ = 0;
  uint64_t frames_z_previous_ = 0;
  std::chrono::steady_clock::time_point last_report_z_ = std::chrono::steady_clock::now();
  bool z_early_ = true;
  bool alternation_z_recorded_ = false;
  bool skip_invisibles_ = true;
  uint64_t shadows_off_alpha_ = 0;  // ... and without it (the stage would be unnecessary)
  std::array<uint64_t, kGpuCategories> triangles_per_category_{};
  std::array<uint64_t, kGpuCategories> passes_per_category_{};
  uint64_t ns_render_pass_ = 0;
  // Diagnostic masseffect_native_diag_vertices_repeated.
  struct VerticesSeen {
    uint64_t frame = UINT64_MAX;
    uint64_t fingerprint = 0;
  };
  std::unordered_map<uint64_t, VerticesSeen> vertices_seen_;
  uint64_t bytes_repeated_frame_ = 0;
  uint64_t bytes_recorded_ = 0;
  std::chrono::steady_clock::time_point repeated_report_ = std::chrono::steady_clock::now();
  uint64_t bytes_equal_previous_ = 0;
  uint64_t ns_hash_vertices_ = 0;
  // Last texture and sampler result of each PS sampler register.
  struct CacheSampler {
    std::array<uint32_t, 6> fetch{};
    uint64_t frame = UINT64_MAX;
    uint64_t generation = 0;
    uint32_t slot = 0;
    uint32_t heap = 0;
    uint32_t sampler = 0;
    uint64_t valid_until = 0;  // last frame it is valid without going back to PrepareTexture
    uint32_t width = 0;  // host image size, for 1/size
    uint32_t height = 0;
  };
  bool cache_between_frames_ = true;  // masseffect_native_cache_textures_between_frames
  bool mipmaps_ = true;                 // masseffect_native_mipmaps
  std::array<CacheSampler, 16> cache_samplers_{};
  // Second cache, by the whole fetch constant: a direct-mapped table (the hash picks the slot). It went
  // from 256 to 1024 slots because in play there are ~340 distinct textures per frame and 256 slots
  // collided (64 KB: fits in L2).
  /*
   * From 1024 to 4096 (288 KB, still fits in the 2 MB L2). In a busy scene there are ~3,000
   * draws per frame and ~205 misses of this table per frame (C6 counters: 97,709 in 20 s), at ~15 us
   * each through PrepareTexture (the texture stage measures 1.2-1.5 us per draw with 3.4 % misses). With
   * N distinct fetches in M slots, the fraction sharing a slot with another is 1 - e^(-N/M): with ~600
   * that is 44 % at 1024 and 14 % at 4096. The hit conditions do not change: there is just more room. The
   * misses-by-cause report (every 10 s) says how many were collisions.
   */
  std::array<CacheSampler, 4096> cache_fetch_{};
  uint64_t samplers_cache_fetch_ = 0;
  uint64_t fetch_failures_clash_ = 0;      // The slot held another fetch constant
  uint64_t fetch_empty_failures_ = 0;       // slot not used yet
  uint64_t fetch_failures_generation_ = 0;  // same fetch, but generation_textures_ changed
  uint64_t fetch_expired_failures_ = 0;    // same fetch, but the texture is due for a check (valid_until)
  std::array<uint64_t, 6> fetch_previous_report_{};
  std::chrono::steady_clock::time_point fetch_report_{};
  uint64_t generation_textures_ = 0;  // changes with every render target copy and every retired image
  // On-disk pipeline cache (LoadCachePipelines and SaveCachePipelines).
  using FnCreateCachePipelines = VkResult(VKAPI_PTR*)(VkDevice, const VkPipelineCacheCreateInfo*,
                                                     const VkAllocationCallbacks*,
                                                     VkPipelineCache*);
  using FnDataCachePipelines = VkResult(VKAPI_PTR*)(VkDevice, VkPipelineCache, size_t*, void*);
  using FnDestroyCachePipelines = void(VKAPI_PTR*)(VkDevice, VkPipelineCache,
                                                    const VkAllocationCallbacks*);
  FnDataCachePipelines data_cache_ = nullptr;
  FnDestroyCachePipelines destroy_cache_ = nullptr;
  VkPipelineCache cache_pipelines_ = VK_NULL_HANDLE;
  uint32_t pipelines_no_save_ = 0;
  uint32_t saved_cache_ = 0;
  size_t bytes_cache_saved_ = 0;  // size of the last file read or saved
  std::chrono::steady_clock::time_point cache_saved_{};
  // Thread that writes the pipeline cache to disk (WriterCacheMain). It is persistent, because on
  // Horizon detach() closes the game, and wake-ups are decided with the lock held.
  std::thread writer_cache_;
  std::mutex writer_mutex_;
  std::condition_variable writer_warning_;
  std::vector<uint8_t> writer_data_;
  bool pending_writer_ = false;
  bool writer_stop_ = false;
  // The single pipelines file. The last written version of each part (or the one read at start-up): owned
  // by the writer thread once it is running, and by Initialize before that. read_list_ goes from
  // LoadCachePipelines to LoadPipelinesList.
  std::vector<uint8_t> written_cache_;
  std::vector<uint8_t> written_list_;
  std::vector<uint8_t> read_list_;
  bool old_files_ = false;  // the two older files were read: deleted when the new one is written
  uint64_t ns_pipelines_ = 0;  // creating pipelines (C6 report)
  std::unordered_set<uint32_t> warned_;
  // Pipeline prewarm (PrewarmedLoop). file_list_ and state_list_ are sized before the thread
  // starts and never change size; the thread writes state_list_[i] before publishing
  // prewarmed_until_ > i. Anything not atomic and not marked otherwise belongs to the ring only.
  std::vector<RegisterPipeline> file_list_;         // the list read at start-up (walked by the thread)
  std::vector<uint8_t> state_list_;                   // kList* of each record of file_list_
  std::unordered_map<uint64_t, size_t> index_list_;   // XXH3 of the key -> index (SIZE_MAX: from this session)
  std::vector<RegisterPipeline> session_list_;          // this session's new ones
  uint32_t list_no_save_ = 0;
  std::vector<uint8_t> writer_list_;                 // guarded by writer_mutex_
  std::thread prewarmed_thread_;
  std::thread preload_thread_;  // TryPreloadShaders
  std::atomic<bool> preload_stop_{false};
  bool decided_preload_ = false;  // ring only
  const ShadersNative* prewarmed_library_ = nullptr;
  uint32_t prewarmed_eds_ = 0;
  bool prewarmed_decided_ = false;
  bool prewarmed_difference_ = false;
  std::chrono::steady_clock::time_point prewarmed_start_{};
  std::chrono::steady_clock::time_point prewarmed_report_{};
  uint64_t prewarmed_previous_changes_ = 0;
  std::atomic<bool> prewarmed_stop_{false};
  std::atomic<bool> prewarmed_finished_{false};
  std::atomic<size_t> prewarmed_until_{0};
  std::atomic<uint32_t> prewarmed_done_{0};
  std::atomic<uint32_t> prewarmed_compiled_{0};
  std::atomic<uint64_t> prewarmed_ns_compiled_{0};
  std::atomic<uint32_t> prewarmed_no_shader_{0};
  std::atomic<uint32_t> prewarmed_other_mode_{0};
  std::atomic<uint32_t> prewarmed_failed_{0};
  std::atomic<int32_t> prewarmed_priority_{-1};
  uint32_t prewarmed_counted_ = 0;  // of prewarmed_compiled_, already added to pipelines_no_save_
  uint64_t prewarmed_ring_ = 0, prewarmed_slow_ring_ = 0, ns_prewarmed_ring_ = 0;
  uint64_t ring_of_list_ = 0, ns_ring_of_list_ = 0;
  uint64_t new_ring_ = 0, ns_new_ring_ = 0, ring_to_list_ = 0;
};

}  // namespace

std::unique_ptr<DrawsVulkan> DrawsVulkan::Create(const VulkanDevice* vulkan_device,
                                                    rex::memory::Memory* memory,
                                                    ContextTargets* context) {
  if (!vulkan_device || !memory || !context) {
    return nullptr;
  }
  auto draws = std::make_unique<DrawsVulkanImpl>(vulkan_device, memory, context);
  if (!draws->Initialize()) {
    REXLOG_ERROR("[native] C6: could not prepare the native draws");
    return nullptr;
  }
  REXLOG_INFO("[native] C6: native draws prepared");
  return draws;
}

}  // namespace masseffect::native
