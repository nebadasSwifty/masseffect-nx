// The native graphics system: an IGraphicsSystem that the app installs in config.graphics, so the Xenos emulation
// plugin is not loaded and no guest memory is watched. The game keeps its own Direct3D; a ring thread reads the PM4
// command list it writes, keeps a copy of the GPU registers, returns the read pointer, delivers interrupts, writes
// SCRATCH_REG/MEM_WRITE/EVENT_WRITE values the game waits for, and counts draws, copies and Swaps. Draws, resolves
// and Swaps are handed to the modules in src/native/masseffect (TargetsNative, DrawsVulkan, ShadersNative)
// with the native shader package (masseffect_shaders.mesp next to the executable).
//
// The app uses it when masseffect_renderer_native is true (see masseffect_app.h).

#include "me_native_system.h"
#include "me_packaged.h"
#include "me_shader_identity.h"
#include "me_shader_candidate_policy.h"
#include "me_native_draw_extent_estimator.h"
#include "me_pm4_runs.h"
#include "me_object_table.h"
#include "me_record_table.h"
#include "me_native_ps_no_kill.h"
#include "me_shader_load_memo.h"
#include "me_ring_partition.h"
#include "me_frame_coherence.h"
#include "me_ring_split.h"
#include "me_texture_coherency.h"  // masseffect_native_texture_coherency

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
#include <tuple>
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
REXCVAR_DECLARE(int32_t, masseffect_scene_width);   // me_resolution.cpp
REXCVAR_DECLARE(int32_t, masseffect_scene_height);  // me_resolution.cpp
REXCVAR_DECLARE(int32_t, masseffect_scene_parts);   // me_resolution.cpp

// D3D9 occlusion queries (EVENT_WRITE_ZPD): docs/occlusion-queries.md.
REXCVAR_DEFINE_INT32(masseffect_native_query_mode, 0, "Mass Effect",
                     "Occlusion query results: 0 = every query reports 1000 samples (visible, as before); 1 = "
                     "diagnostic, every query reports 0 samples (everything occlusion-tested is hidden: wrong image, "
                     "bounds the gain); 2 = real Vulkan occlusion queries around the draws of each query, written "
                     "into guest memory when the GPU has them; 3 = latency-1 real queries: answered at once from the "
                     "most recent GPU result of the same query identity (unknown or stale = visible), while the GPU "
                     "measures the current issue for the next ones (never waited for)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_query_skip_boxes, false, "Mass Effect",
                    "With query modes 0 and 1: draws between an occlusion query's begin and end that cannot change "
                    "any pixel (no color writes, no depth writes, no stencil) are not recorded at all; they only "
                    "fed the query, whose result is not measured. Ignored in mode 2")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_query_pair_by_address, true, "Mass Effect",
                    "Occlusion queries, all modes: an EVENT_WRITE_ZPD whose address + 0x20 is the begin structure "
                    "of the open query is its END even when the end structure no longer holds D3D's sentinel. "
                    "D3D stores the sentinel at Issue(BEGIN) on the CPU; when the ring lags, writing the previous "
                    "issue's result erases the sentinel of the next one, and the END was then taken for a BEGIN and "
                    "zeroed (the game read 0 samples and culled). docs/image-defects-feros.md 3.8.2")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_query_flush_us, 500, "Mass Effect",
                     "Query mode 2: when the ring has nothing left to parse and a finished query has waited this "
                     "long in the work being recorded, submit that work early (the game is probably polling the "
                     "query); -1 = never, results then wait for the Swap's submission")
    .range(-1, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_query_timeout_us, 50000, "Mass Effect",
                     "Query mode 2: a query without a GPU result after this long is answered 'visible' (a game "
                     "waiting for it can never hang); 0 = no timeout")
    .range(0, 10000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_query_max_age, 4, "Mass Effect",
                     "Query mode 3: a history result whose issue ended more than this many Swaps ago is not used "
                     "(the query is answered 'visible')")
    .range(1, 120)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_query_hidden_after, 2, "Mass Effect",
                     "Query mode 3: a query is answered 0 samples (hidden) only after this many consecutive zero "
                     "results of its identity; any non-zero result in between answers 'visible' (limits pop-in and "
                     "flicker). 1 = the last result alone decides")
    .range(1, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_query_history_key, 0, "Mass Effect",
                     "Query mode 3, identity of a query in the history table: 0 = end structure address + hash of "
                     "the box vertex data and target (safe if UE3 pools its query objects; fewer hits if the pool "
                     "reshuffles); 1 = box content hash only (survives pooling; wrong only if two primitives share "
                     "identical box data); 2 = end structure address only (wrong if query objects are pooled)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

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
REXCVAR_DEFINE_BOOL(masseffect_native_load_memo, false, "Mass Effect",
                    "Ring: a shader IM_LOAD whose raw guest words equal the last load from the same address and "
                    "size (one memcmp) reuses that load's byte-swapped microcode and XXH3 for the identity memo "
                    "instead of swapping and hashing again; exact (me_shader_load_memo.h, self-checked for the "
                    "first masseffect_native_verify_n hits)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_load_memo_generation, false, "Mass Effect",
                    "Ring, with masseffect_native_load_memo: a load memo hit restores the identity generation its "
                    "words had when stored, so the per-draw identity checks memoized for those exact words stay "
                    "valid when shaders alternate (A, B, A); exact (the results depend only on the words)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_ring_partition, false, "Mass Effect",
                    "Ring (measurement only): split the ring thread's time into phases (wait, PM4 parse, registers, "
                    "shader loads, pairing, EDRAM prepare / transfers / publish, Vulkan draw, textures, copies, "
                    "present, ...) with the ARM counter and report ms per 10 s and us per ring draw "
                    "(me_ring_partition.h). Costs ~0.3-0.6 us per draw while on")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_coherence_stats, false, "Mass Effect",
                    "Ring (measurement only): frame coherence of ring draws. Keys per draw (shaders, render state, "
                    "texture fetch words, shader constants, index/vertex ranges and fingerprints) are compared with "
                    "the previous frame; every 10 s two 'frame coherence' lines give the share of draws that match "
                    "(same ordinal / any position) and the ring time and C6 stages of the matching draws "
                    "(me_frame_coherence.h, docs/frame-coherence.md)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_coherence_every, 8, "Mass Effect",
                     "masseffect_native_coherence_stats: record frame pairs every N frames (frames f % N == 0 and 1; "
                     "the second is compared with the first). 1 = every frame")
    .range(1, 1024)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_effects_stats, false, "Mass Effect",
                    "Ring (measurement only, step 0 of docs/multithread-translation.md): count the guest-visible "
                    "effects (fences, MEM_WRITE, REG_TO_MEM, COND_WRITE, EVENT_WRITE_EXT, scratch write-back, "
                    "interrupts, occlusion writes, read-pointer write-back) and WAIT_REG_MEM sync points, and the ring "
                    "draws between consecutive ones; one 'guest-visible effects' line every 10 s (me_ring_split.h)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_split_lockstep, false, "Mass Effect",
                    "Ring (step 1 of docs/multithread-translation.md): the front journals every register store and "
                    "microcode change; the back (draws, copies, Swaps behind TargetsNative) applies the journal to "
                    "its own register/microcode mirror and reads only the mirror. Same thread, same order (no second "
                    "thread); exact. One 'ring split (lockstep)' line every 10 s with the journal's cost")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_split_verify, 4096, "Mass Effect",
                     "masseffect_native_split_lockstep: compare the back's mirror with the ring's registers (all "
                     "0x5003 words) and microcode bit for bit at the first N back operations; any difference logs "
                     "DIFFERENCE and turns the split off for the session (the back then reads the live registers "
                     "again). 0 = no check")
    .range(0, 1000000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_split_verify_every, 1024, "Mass Effect",
                     "masseffect_native_split_lockstep: after the first masseffect_native_split_verify back "
                     "operations, compare one in N; 0 = never")
    .range(0, 1000000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_vblank_adaptive, false, "Mass Effect",
                    "Adaptive VBlank: when a frame is already late (two VBlank periods since the previous Swap) and the "
                    "GPU waits on memory for the game's VBlank handler, fire the VBlank now instead of at the next "
                    "60 Hz tick. A late frame then costs its own time instead of a whole extra VBlank (35 ms -> 50 ms). "
                    "Frames on time keep the 30 fps pacing. The output is presented immediately anyway");
REXCVAR_DEFINE_INT32(masseffect_native_waitregmem_spin_us, 0, "Mass Effect",
                     "PM4 WAIT_REG_MEM: poll with yields for up to this many microseconds before the 50 us sleeps "
                     "(the Horizon sleep lasts much longer than asked; ~0.35 ms per wait measured); 0 = sleep at once")
    .range(0, 5000);
REXCVAR_DEFINE_BOOL(masseffect_native_waitregmem_stats, false, "Mass Effect",
                    "Diagnostics: log every 10 s which WAIT_REG_MEM conditions the ring thread waited on (register or "
                    "memory address, reference, mask), how often and for how long");
REXCVAR_DEFINE_BOOL(masseffect_native_mismatch_log_info, false, "Mass Effect",
                    "Ring: the periodic VS/PS identity mismatch lines (1 in 256 after the first 32, ~110 per 10 s "
                    "in the Normandy walk) are written at info instead of warn level. Warn lines flush the log "
                    "file to the SD card on the ring thread (flush_on(warn)); the text is the same")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_verify_n, 2048, "Mass Effect",
                     "Self-check of the switches above: the old and the new path are both evaluated for the first N "
                     "uses and compared; any difference logs DIFFERENCE and turns that switch off. 0 = no check")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(masseffect_diag_missing_shader_draws, false, "Mass Effect",
                    "Diagnostics (docs/image-defects-feros.md, 'Overheat bar missing'): for draws dropped because "
                    "the VS or PS is not in the shader package, log the draw state once per distinct state "
                    "(primitive, count, color mask, blend, alpha test, depth/stencil, target, texture 0) and the "
                    "full microcode of each missing program once, so it can be wrapped into "
                    "shaders/runtime_containers. Limits: masseffect_diag_missing_shader_states_max and "
                    "masseffect_diag_missing_shader_programs_max. No image change");
REXCVAR_DEFINE_INT32(masseffect_diag_missing_shader_states_max, 64, "Mass Effect",
                     "masseffect_diag_missing_shader_draws: at most N distinct 'missing shader draw' state lines per "
                     "run. 0 = no limit (one line per distinct state)")
    .range(0, 1000000);
REXCVAR_DEFINE_INT32(masseffect_diag_missing_shader_programs_max, 0, "Mass Effect",
                     "masseffect_diag_missing_shader_draws: at most N missing programs get their microcode dumped "
                     "('missing shader microcode' lines), each once per run. 0 = no limit (every distinct missing "
                     "VS/PS once). The old fixed limit was 16")
    .range(0, 1000000);
REXCVAR_DEFINE_BOOL(masseffect_native_vs_identify_patched, true, "Mass Effect",
                    "Vertex shaders the exact library lookup misses are looked up again with the fetch destination "
                    "swizzles Direct3D patches per vertex declaration left out (FLOAT3 position 688 -> A88, "
                    "D3DCOLOR texcoord E88 -> E0A). Exact otherwise: every ALU/CF word and every other fetch field "
                    "identical, every swizzle change representable by the input remap (the draw-time identity "
                    "test). Without it such draws are dropped (BDtS asteroid X57 VS 83D232C56BD49D57 and "
                    "0D0AB386D5018592, Normandy/Wards VS 30458CCA865ABD51). Report line: 'VS patched-fetch lookup'")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_vs_fetch_permutation, true, "Mass Effect",
                    "Vertex shader identity also accepts Direct3D's reorder of back-to-back fetches into the same "
                    "temporary register (disjoint components, one exec clause; e.g. Eden Prime VS CD057930742AFE84, "
                    "r3.xy and r3.zw swapped). Every other word stays exact; the vertex input then takes each "
                    "element from the fetch of its run that writes its components")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_vs_identity_tolerance, 0, "Mass Effect",
                     "Experimental: a vertex shader the ring loaded may differ from its library candidate (same size, "
                     "fetches still checked) in up to N ALU/CF words and still be drawn with that candidate. 0 = "
                     "exact identity (the draw is dropped when no candidate matches, as before). Try 4-16 when the "
                     "log shows 'unresolved draw shader pairs' for a VS that masseffect_native_vs_identify_patched "
                     "does not find (CD057930742AFE84 is now found by the fetch permutation); see the "
                     "'[native] VS word diff' lines")
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
constexpr uint32_t kRegCoherSizeHost = 0x0A2F;  // bytes (D3D rounds them up to 4 KB)
constexpr uint32_t kRegCoherBaseHost = 0x0A30;  // physical address (D3D rounds it down to 4 KB)
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
static_assert(kRegisterCount >= me::native::coherence::kRegMinRequired, "frame coherence reads 0x4900-0x4927");
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
    g_vs_identity_fetch_permutation.store(REXCVAR_GET(masseffect_native_vs_fetch_permutation),
                                          std::memory_order_relaxed);
    shaders_.SetPatchedLookup(REXCVAR_GET(masseffect_native_vs_identify_patched));
    identity_flat_ = REXCVAR_GET(masseffect_native_flat_identity);
    g_record_fast = pairing_fast_ = REXCVAR_GET(masseffect_native_fast_pair);
    raw_microcode_ = REXCVAR_GET(masseffect_native_raw_microcode);
    pm4_fast_ = REXCVAR_GET(masseffect_native_pm4_fast);
    query_mode_ = REXCVAR_GET(masseffect_native_query_mode);
    query_skip_boxes_ = REXCVAR_GET(masseffect_native_query_skip_boxes);
    query_pair_by_address_ = REXCVAR_GET(masseffect_native_query_pair_by_address);
    query_flush_us_ = REXCVAR_GET(masseffect_native_query_flush_us);
    query_timeout_us_ = uint32_t(std::max<int32_t>(0, REXCVAR_GET(masseffect_native_query_timeout_us)));
    query_max_age_ = uint32_t(std::max<int32_t>(1, REXCVAR_GET(masseffect_native_query_max_age)));
    query_hidden_after_ = uint32_t(std::max<int32_t>(1, REXCVAR_GET(masseffect_native_query_hidden_after)));
    query_history_key_ = REXCVAR_GET(masseffect_native_query_history_key);
    {
      // The game draws its scene at masseffect_scene_width x _height when the viewport part is on
      // (me_resolution.cpp): scale the counts so UE3's thresholds see the numbers of 1280x720.
      const int32_t w = REXCVAR_GET(masseffect_scene_width), h = REXCVAR_GET(masseffect_scene_height);
      if (w > 0 && h > 0 && (REXCVAR_GET(masseffect_scene_parts) & 1)) query_scale_ = (1280.0 * 720.0) / (double(w) * h);
    }
    if (query_mode_ || query_skip_boxes_)
      REXLOG_INFO("[native] occlusion queries: mode {} ({}), skip boxes {}{}, flush {} us, timeout {} us, count scale {:.3f}",
                  query_mode_,
                  query_mode_ == 0   ? "visible"
                  : query_mode_ == 1 ? "DIAGNOSTIC: all hidden"
                  : query_mode_ == 2 ? "real"
                                     : "latency-1 real",
                  query_skip_boxes_, query_mode_ >= 2 && query_skip_boxes_ ? " (ignored in modes 2 and 3)" : "",
                  query_flush_us_, query_timeout_us_, query_scale_);
    if (query_mode_ == 3)
      REXLOG_INFO("[native] occlusion queries, latency-1: max age {} Swaps, hidden after {} zero results, identity "
                  "{} (flush and timeout unused)",
                  query_max_age_, query_hidden_after_,
                  query_history_key_ == 0   ? "address + box content"
                  : query_history_key_ == 1 ? "box content"
                                            : "address");
    check_identity_ = check_pairing_ = check_raw_ = check_pm4_ = check_objects_ = check_load_memo_ =
        uint32_t(std::max<int32_t>(0, REXCVAR_GET(masseffect_native_verify_n)));
    if (REXCVAR_GET(masseffect_native_load_memo)) {
      load_memo_ = std::make_unique<ShaderLoadMemo>();
      load_memo_generation_ = REXCVAR_GET(masseffect_native_load_memo_generation);
      REXLOG_INFO("[native] ring CPU switch: shader load memo by address on ({} slots), identity generations "
                  "restored on a hit {}", ShaderLoadMemo::kSlots, load_memo_generation_);
    }
    if (identity_flat_ || pairing_fast_ || raw_microcode_ || pm4_fast_)
      REXLOG_INFO("[native] ring CPU switches: flat identity {}, fast pairing {}, raw microcode compare {}, fast PM4 "
                  "runs {}; self-check of the first {} uses of each", identity_flat_, pairing_fast_, raw_microcode_,
                  pm4_fast_, check_identity_);
    effects_on_ = REXCVAR_GET(masseffect_native_effects_stats);
    if (REXCVAR_GET(masseffect_native_split_lockstep)) {
      // MMIO writes from game threads are not in the journal: they are flagged and re-journaled at the next sync.
      mmio_touched_ = std::make_unique<std::atomic<uint8_t>[]>(kRegisterCount);
      for (uint32_t i = 0; i < kRegisterCount; ++i) mmio_touched_[i].store(0, std::memory_order_relaxed);
      split_on_ = true;
      split_mmio_.store(true, std::memory_order_release);
      split_verify_.first = uint64_t(REXCVAR_GET(masseffect_native_split_verify));
      split_verify_.every = uint64_t(REXCVAR_GET(masseffect_native_split_verify_every));
      REXLOG_INFO("[native] ring split: lockstep (journal + back mirror on the ring thread); verification of the first "
                  "{} back operations, then 1 in {}", split_verify_.first, split_verify_.every);
    }
    if (effects_on_) REXLOG_INFO("[native] guest-visible effects statistics on (measurement): report every 10 s");
    g_native_active.store(true, std::memory_order_release);
    const char* lib = std::getenv("MASSEFFECT_SHADER_LIBRARY");
    const auto lib_path = lib && *lib ? std::filesystem::path(lib)
                                      : me::packaged::DataFile("masseffect_shaders.mesp");  // RomFS in an installed NSP
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
    if (r == kRegCoherBaseHost) {  // masseffect_native_texture_coherency (not expected: D3D uses the ring)
      me::native::texture_coherency::Mark(value, registers_[kRegCoherSizeHost],
                                          me::native::texture_coherency::kSourceMmio);
    }
    if (r < kRegisterCount && r != kRegCpRbWptr && split_mmio_.load(std::memory_order_acquire)) {
      // masseffect_native_split_lockstep: the ring re-journals this register's live value at its next sync.
      if (!mmio_touched_[r].exchange(1, std::memory_order_relaxed)) mmio_distinct_.fetch_add(1, std::memory_order_relaxed);
      mmio_epoch_.fetch_add(1, std::memory_order_release);
    }
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
    if (index == kRegCoherBaseHost) {
      // masseffect_native_texture_coherency: D3D writes SIZE then BASE in one type-0 run (sub_82227210), so the
      // size is already in place. The WAIT_REG_MEM on the status marks again with the final pair.
      me::native::texture_coherency::Mark(value, registers_[kRegCoherSizeHost],
                                          me::native::texture_coherency::kSourceBaseWrite);
    }
    if (split_on_) {  // masseffect_native_split_lockstep: journal (the gamma path also advances DC_LUT_RW_INDEX)
      if (index >= kRegGammaFirst && index <= kRegGammaLast) journal_.Reg(kRegGammaIndex, registers_[kRegGammaIndex]);
      journal_.Reg(index, value);
    }
    if (index >= kRegScratch0 && index <= kRegScratch7) {
      const uint32_t n = index - kRegScratch0;
      if ((1u << n) & registers_[kRegScratchUmsk]) {
        rex::memory::store_and_swap<uint32_t>(
            memory_->TranslatePhysical(registers_[kRegScratchAddr] + n * 4), value);
        if (effects_on_) effects_.Note(me::native::ring_split::kScratch);
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
    if (split_on_) journal_.Regs(index, regs + index, count);
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
    if (split_on_) journal_.Regs(index, regs + index, count);
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

  static uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
  }
  // masseffect_vblank_adaptive: called by the ring thread while a WAIT_REG_MEM on memory is not met.
  void RequestEarlyVblank(Clock::time_point start_wait) {
    const uint64_t interval = vblank_interval_ns_.load(std::memory_order_acquire);
    const uint64_t last = last_swap_ns_.load(std::memory_order_acquire);
    if (!interval || !last) return;
    const uint64_t now = NowNs();
    // Only for a frame that is already late, only once per Swap, and only after the wait is not trivially short.
    if (now - last < 2 * interval - 500000 || early_requested_swap_ == swaps_) return;
    if (Clock::now() - start_wait < std::chrono::microseconds(200)) return;
    early_requested_swap_ = swaps_;
    {
      std::lock_guard<std::mutex> lock(vblank_mutex_);
      vblank_early_.store(true, std::memory_order_release);
    }
    vblank_cv_.notify_one();
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    vblank_interval_ns_.store(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count()),
                              std::memory_order_release);
    adaptive_vblank_.store(REXCVAR_GET(masseffect_vblank_adaptive), std::memory_order_release);
    if (adaptive_vblank_.load()) REXLOG_INFO("[native] adaptive VBlank on: late frames fire the VBlank when the GPU waits for it");
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      if (now - next > std::chrono::milliseconds(250)) next = now;
      if (vblank_early_.exchange(false, std::memory_order_acq_rel)) {
        // masseffect_vblank_adaptive: the GPU waits for a late frame's VBlank; fire it now and restart the phase.
        counter_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        ++vblanks_early_;
        next = now + interval;
      }
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      if (adaptive_vblank_.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(vblank_mutex_);
        vblank_cv_.wait_until(lock, next, [this] {
          return vblank_early_.load(std::memory_order_acquire) || !active_.load(std::memory_order_acquire);
        });
      } else if (REXCVAR_GET(masseffect_vblank_sleep_exact)) {
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
    namespace partition = me::native::ring_partition;
    partition::BindThisThread(REXCVAR_GET(masseffect_native_ring_partition));
    me::native::coherence::Configure(REXCVAR_GET(masseffect_native_coherence_stats),  // frame coherence (measurement)
                                     uint32_t(REXCVAR_GET(masseffect_native_coherence_every)));
    if (me::native::coherence::g.on)
      REXLOG_INFO("[native] frame coherence on (measurement): frame pairs every {} frames, report every 10 s",
                  me::native::coherence::g.every);
    if (partition::g_state.on)
      REXLOG_INFO("[native] ring partition on (measurement): phases every 10 s, ARM counter at {:.1f} MHz",
                  partition::TicksPerSecond() / 1e6);
    if (split_on_) SplitSeed();  // the back's copy starts equal to the live registers and microcode
    while (active_.load(std::memory_order_acquire)) {
      {
        partition::Scope phase_wait(partition::kWait);
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_waiting_.store(true, std::memory_order_seq_cst);
        // Real occlusion queries pending: wake often enough to write their results and time them out.
        const bool queries = query_mode_ == 2 && targets_ && targets_->QueriesPending();
        ring_cv_.wait_for(lock, queries ? std::chrono::microseconds(250) : std::chrono::microseconds(5000), [&] {
          const uint32_t words = ring_words_.load(std::memory_order_acquire);
          return !active_.load(std::memory_order_acquire) ||
                 (words && (write_pointer_.load(std::memory_order_seq_cst) & (words - 1)) != read);
        });
        ring_waiting_.store(false, std::memory_order_relaxed);
      }
      if (!active_.load(std::memory_order_acquire)) break;
      {
        partition::Scope phase_report(partition::kReport);
        Report(false);
      }
      ServiceQueries(read);
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
      partition::Scope phase_flush(partition::kFlush);
      const uint32_t writeback = read_writeback_.load(std::memory_order_acquire);
      if (split_on_) SplitSync();  // end of segment: the journal never outgrows one segment
      if (writeback) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
        if (effects_on_) effects_.Note(me::native::ring_split::kReadPointer);
      }
      NotifyRingProgress();
      masseffect::native::deferred::Flush();  // Batched mode: the worker must not wait for the next WPTR
      ServiceQueries(read);
    }
    return 0;
  }

  // Real occlusion queries: results of finished submissions, an early submission when the ring is idle, and
  // the timeout. Idle = everything the game has written to the ring is parsed.
  void ServiceQueries(uint32_t read) {
    if (query_mode_ < 2 || !targets_ || !targets_->QueriesPending()) return;
    if (query_mode_ == 3) {
      // Latency-1: only fold the results of finished submissions into the history (fence polls, no waiting, no
      // early submission, no timeout), at most once per millisecond.
      const auto now = std::chrono::steady_clock::now();
      if (now - query_serviced_ < std::chrono::microseconds(1000)) return;
      query_serviced_ = now;
      targets_->QueryService(false, 0, 0);
      return;
    }
    const uint32_t words = ring_words_.load(std::memory_order_acquire);
    const bool idle = !words || (write_pointer_.load(std::memory_order_acquire) & (words - 1)) == read;
    targets_->QueryService(idle && query_flush_us_ >= 0, uint32_t(std::max(0, query_flush_us_)), query_timeout_us_);
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
        me::native::ring_partition::Scope phase(me::native::ring_partition::kRegisters);
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
    namespace partition = me::native::ring_partition;
    uint32_t phase = partition::kNone;
    switch (opcode) {
      case xenos::PM4_SET_CONSTANT:
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS:
      case xenos::PM4_LOAD_ALU_CONSTANT: phase = partition::kRegisters; break;
      case xenos::PM4_IM_LOAD:
      case xenos::PM4_IM_LOAD_IMMEDIATE: phase = partition::kShaderLoad; break;
      case xenos::PM4_WAIT_REG_MEM: phase = partition::kWaitRegMem; break;
      case xenos::PM4_XE_SWAP: phase = partition::kPresent; break;
      default: break;
    }
    partition::Scope phase_packet(phase);
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (words < 1) break;
        const uint32_t cpus = data.Read();
        if (effects_on_) effects_.Note(me::native::ring_split::kInterrupt);
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
        if (me::native::coherence::g.on) CoherenceEndFrame();  // frame coherence
        counter_.fetch_add(1, std::memory_order_relaxed);
        last_swap_ns_.store(NowNs(), std::memory_order_release);
        if (effects_on_) effects_.BackWork();
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
        const auto start_wait = Clock::now();
        const auto limit = start_wait + kWaitRegMemMax;
        const int32_t spin_us = REXCVAR_GET(masseffect_native_waitregmem_spin_us);
        bool waited = false;
        for (;;) {
          if (!(info & 0x10) && poll == kRegCoherStatusHost && (registers_[poll] & 0x80000000u)) {
            // masseffect_native_texture_coherency: the range the game declared changed (no-op when off).
            me::native::texture_coherency::Mark(registers_[kRegCoherBaseHost], registers_[kRegCoherSizeHost],
                                                me::native::texture_coherency::kSourceWait);
            registers_[poll] = 0;  // MakeCoherent: no shared memory to synchronize
            if (split_on_) journal_.Reg(poll, 0);
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
          waited = true;
          if ((info & 0x10) && adaptive_vblank_.load(std::memory_order_relaxed)) RequestEarlyVblank(start_wait);
          if (spin_us > 0 && Clock::now() - start_wait < std::chrono::microseconds(spin_us)) {
            std::this_thread::yield();
          } else {
            rex::thread::Sleep(std::chrono::microseconds(50));
          }
        }
        if (effects_on_)
          effects_.Note((info & 0x10) ? me::native::ring_split::kWaitMemory : me::native::ring_split::kWaitRegister);
        if (REXCVAR_GET(masseffect_native_waitregmem_stats)) {
          NoteWaitRegMem(info, poll, ref, mask, waited,
                         uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_wait).count()));
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
        if (effects_on_) effects_.Note(me::native::ring_split::kRegToMem);
        WriteMemory(address, Register(reg));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (words < 1) break;
        uint32_t address = data.Read();
        if (effects_on_) effects_.Note(me::native::ring_split::kMemWrite);
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
            if (effects_on_) effects_.Note(me::native::ring_split::kCondWriteMemory);
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
        if (effects_on_) effects_.Note(me::native::ring_split::kFence);
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
        if (effects_on_) effects_.Note(me::native::ring_split::kEventWriteExt);
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
        // D3D places the byte-swapped sentinel 0xFFFFFEED in the end structure at Issue(BEGIN), on the CPU.
        // GetData: not finished while end words 0..3 all hold it; result = end ZPass - begin ZPass.
        const uint32_t address = Register(kRegRbSampleCountAddr);
        if (address) {
          auto* counts =
              memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
          const uint32_t finished = rex::byte_swap(uint32_t(0xFFFFFEED));
          bool end = counts->ZPass_A == finished || counts->ZPass_B == finished ||
                     counts->ZFail_A == finished || counts->ZFail_B == finished ||
                     counts->Total_A == finished || counts->Total_B == finished;
          // D3D stores the sentinel at Issue(BEGIN) (sub_82228F58 in the English edition), on the CPU. If the game
          // issues the same query object again before this thread has parsed the previous END, writing that
          // result (modes 0, 1, 3) erases the new sentinel: the new END then looked like a BEGIN and its end
          // structure was zeroed (GetData: 0 samples, UE3 culls the primitive or light). Pair by address instead:
          // the open query's begin structure is its end + 0x20 (a BEGIN at that end address would need another
          // query structure overlapping this one).
          // Counted also with the pairing off (then they are the ENDs taken for BEGINs).
          if (!end && query_open_ && address + 0x20 == query_open_begin_) {
            ++query_end_by_address_;
            end = query_pair_by_address_;
          }
          if (effects_on_) effects_.Note(end ? me::native::ring_split::kQueryEnd : me::native::ring_split::kQueryBegin);
          ZpdQuery(address, end);
          static uint32_t query_reports = 0;
          if (query_reports++ < 30) {
            REXLOG_INFO("[native] ZPD {} at {:08X} (mode {}): {} samples{}", end ? "end" : "begin", address,
                        query_mode_, uint32_t(counts->ZPass_A),
                        end && counts->Total_A == finished ? " (pending)" : "");
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
          if (effects_on_) effects_.BackWork();
          partition::Scope phase_copy(partition::kCopy);
          Copy();
        } else {
          ++draws_;
          if (effects_on_) effects_.Draw();
          if (depth > 0) ++draws_indirect_;
          if (query_open_) ++query_draws_;
          if (me::native::coherence::Recording()) me::native::coherence::BeginDraw();  // frame coherence
          {
            partition::Scope phase_pair(partition::kPair);
            PairDraw();
          }
          if (draw_vs_ && (draw_ps_ || (Register(kRegRbModeControl) & 0x7) == 5)) {
            partition::Scope phase_draw(partition::kDrawFront);
            Draw();
          } else {
            ++draws_unidentified_;
            ++missing_draw_pairs_[{draw_vs_ ? 0 : current_vs_hash_, draw_ps_ ? 0 : current_ps_hash_}];
            if (REXCVAR_GET(masseffect_diag_missing_shader_draws)) LogMissingShaderDraw();
          }
          if (me::native::coherence::Recording()) CoherenceEndDraw();  // frame coherence
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
        if (load_memo_ && load_type <= 1 && LoadFromMemo(load_type, src, size)) {
          if (raw_microcode_) {  // as below: the stage's microcode equals this load
            auto& raw = load_type == 1 ? raw_ps_ : raw_vs_;
            raw.assign(reinterpret_cast<const uint32_t*>(src), reinterpret_cast<const uint32_t*>(src) + size);
            (load_type == 1 ? raw_ps_valid_ : raw_vs_valid_) = true;
          }
          break;
        }
        microcode_.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
        }
        identify_hashed_ = false;
        IdentifyShader(load_type);
        if (load_memo_ && load_type <= 1 && identify_hashed_) {
          load_memo_->Store(load_type, im_load_address_, microcode_, identify_hash_,
                            load_type == 1 ? ps_identity_generation_ : vs_identity_generation_);
        }
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
    return VertexShaderProgramMatches(
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
      cached.second = VertexShaderProgramMatches(
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

  // masseffect_native_mismatch_log_info: the periodic mismatch lines (after the first 32) at info level, so they do
  // not flush the log file to the SD card on the ring thread (the logger flushes on warn).
  static spdlog::level::level_enum MismatchLevel(uint64_t mismatch) {
    static const bool info = REXCVAR_GET(masseffect_native_mismatch_log_info);
    return info && mismatch > 32 ? spdlog::level::info : spdlog::level::warn;
  }

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
      REX_LOG_IMPL(::rex::log::core(), MismatchLevel(mismatch), "[native] VS identity mismatch {} draw={} packet={:08X} "
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
      REX_LOG_IMPL(::rex::log::core(), MismatchLevel(mismatch), "[native] PS identity mismatch {} draw={} packet={:08X} "
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

  // The identity memo hit of IdentifyShader: the stage takes these words, hash and entry.
  // generation: 0 = a new one; otherwise the generation these exact words had when they were stored in the load
  // memo (masseffect_native_load_memo_generation).
  void ApplyMemoHit(uint32_t type, const std::vector<uint32_t>& words, uint64_t memo_hash,
                    const masseffect::native::ShaderEntry* entry, uint64_t generation = 0) {
    ++microcode_epoch_[type == 1 ? 1 : 0];  // masseffect_native_split_lockstep
    if (type == 1) {
      ps_microcode_ = words;
      NewIdentityGeneration(ps_identity_generation_, generation);
      current_ps_hash_ = memo_hash;
      ps_ = entry;
    } else {
      vs_microcode_ = words;
      current_vs_hash_ = memo_hash;
      NewIdentityGeneration(vs_identity_generation_, generation);
      vs_ = entry;
      vs_patched_ = entry && !vs_patched_hashes_.empty() && vs_patched_hashes_.count(memo_hash) != 0;
      gen_vs_ = ++gen_microcode_;
    }
    ++shaders_identified_;
    ++identity_memo_hits_;
  }

  // masseffect_native_load_memo: IdentifyShader for a load whose raw words equal the last load from the same
  // address and size (me_shader_load_memo.h), without the byte swap and the XXH3. It takes exactly the branches
  // IdentifyShader would take (repeated identity, then the identity memo) and returns false, having changed
  // nothing, when IdentifyShader would go further (memo miss): the caller then runs the usual path.
  bool LoadFromMemo(uint32_t type, const uint8_t* src, uint32_t size) {
    if (!im_load_address_) return false;  // address 0: never stored
    const ShaderLoadMemo::Slot* slot = load_memo_->Find(type, im_load_address_, src, size);
    if (!slot) return false;
    if (check_load_memo_ > 0) {
      --check_load_memo_;
      bool equal = slot->swapped.size() == size;
      for (uint32_t i = 0; equal && i < size; ++i)
        equal = slot->swapped[i] == rex::memory::load_and_swap<uint32_t>(src + size_t(i) * 4);
      if (!equal || XXH3_64bits(slot->swapped.data(), size_t(size) * sizeof(uint32_t)) != slot->memo_hash) {
        REXLOG_ERROR("[native] DIFFERENCE: shader load memo ({} words, type {}, address {:08X}) does not match "
                     "the swapped words or their hash; the old path from now on", size, type, im_load_address_);
        load_memo_.reset();
        return false;
      }
    }
    if (REXCVAR_GET(masseffect_native_repeated_identity)) {
      const auto& previous = type == 1 ? ps_microcode_ : vs_microcode_;
      const auto* previous_entry = type == 1 ? ps_ : vs_;
      if (previous_entry && previous.size() == size &&
          (type == 1 ? last_ps_load_address_ : last_vs_load_address_) == im_load_address_ &&
          std::memcmp(previous.data(), slot->swapped.data(), size_t(size) * sizeof(uint32_t)) == 0) {
        ++shader_loads_reused_;
        return true;
      }
    }
    if (!REXCVAR_GET(masseffect_native_identity_memo)) return false;
    const uint64_t memo_key = slot->memo_hash ^ (uint64_t(type) << 63) ^ (uint64_t(size) << 40);
    const auto it = identity_memo_.find(memo_key);
    if (it == identity_memo_.end()) return false;
    (type == 1 ? last_ps_load_address_ : last_vs_load_address_) = im_load_address_;
    ApplyMemoHit(type, slot->swapped, slot->memo_hash, it->second,
                 load_memo_generation_ ? slot->generation : 0);
    ++load_memo_hits_;
    return true;
  }

  /*
   * The identity cells (MatchesVertexShaderIdentity, MatchesPixelShaderIdentity, FetchCoherent) are memoized per
   * stage "generation", and their results are pure functions of (candidate, stage microcode). By default every
   * load that is not an identical reload takes a new generation, so a draw sequence A, B, A recomputes the word
   * compares of A's candidates although A's words are back. masseffect_native_load_memo_generation: generations
   * come from one counter (always fresh, never reused for other words), and a load memo hit restores the
   * generation its words had when they were stored: the slot's words are, word for word, the words the stage held
   * under that generation (Find compared them), so every cell computed under it is still right.
   */
  void NewIdentityGeneration(uint64_t& stage_generation, uint64_t restored) {
    if (!load_memo_generation_) {
      ++stage_generation;
      return;
    }
    stage_generation = restored ? restored : ++identity_generation_counter_;
  }

  // masseffect_diag_missing_shader_draws: what a draw dropped for a missing VS/PS would have drawn, and the
  // exact words of the missing program (host order, the same words MASSEFFECT_SHADER_DISCOVERY writes).
  // Ring thread only; the sets are bounded by the two *_max cvars (0 = one entry per distinct state/program).
  void LogMissingShaderDraw() {
    namespace gr = rex::graphics;
    const uint32_t initiator = Register(kRegVgtDrawInitiator);
    const uint32_t* fetch = registers_.data() + kRegFetchFirst;  // texture fetch constant 0 (6 words)
    const uint32_t color_info = Register(gr::XE_GPU_REG_RB_COLOR_INFO);
    const uint32_t state[] = {
        initiator & 0xFFFF, Register(kRegRbModeControl), Register(gr::XE_GPU_REG_RB_COLOR_MASK),
        Register(gr::XE_GPU_REG_RB_BLENDCONTROL0), Register(gr::XE_GPU_REG_RB_COLORCONTROL),
        Register(gr::XE_GPU_REG_RB_DEPTHCONTROL), Register(gr::XE_GPU_REG_RB_STENCILREFMASK), color_info,
        Register(gr::XE_GPU_REG_RB_SURFACE_INFO), fetch[1] & 0x3F, fetch[2]};
    const uint64_t key = XXH3_64bits(state, sizeof(state)) ^ current_vs_hash_ ^ (current_ps_hash_ << 1) ^
                         (draw_vs_ ? 1 : 0) ^ (draw_ps_ ? 2 : 0);
    static std::unordered_set<uint64_t> states_seen;
    const size_t states_max = size_t(REXCVAR_GET(masseffect_diag_missing_shader_states_max));
    if ((!states_max || states_seen.size() < states_max) && states_seen.insert(key).second) {
      REXLOG_INFO("[native] missing shader draw: VS {:016X}{} ({} words) PS {:016X}{} ({} words) | prim {} count {} "
                  "mode {} | color mask {:04X} blend0 {:08X} colorcontrol {:08X} alpha ref {:08X} | depthcontrol "
                  "{:08X} stencil {:08X} | color info {:08X} surface {:08X} | tex0 format {} {}x{} base {:08X} "
                  "words {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} | draw {}",
                  current_vs_hash_, draw_vs_ ? " found" : " MISSING", vs_microcode_.size(), current_ps_hash_,
                  draw_ps_ ? " found" : " MISSING", ps_microcode_.size(), initiator & 0x3F, initiator >> 16,
                  Register(kRegRbModeControl) & 7, Register(gr::XE_GPU_REG_RB_COLOR_MASK),
                  Register(gr::XE_GPU_REG_RB_BLENDCONTROL0), Register(gr::XE_GPU_REG_RB_COLORCONTROL),
                  Register(gr::XE_GPU_REG_RB_ALPHA_REF), Register(gr::XE_GPU_REG_RB_DEPTHCONTROL),
                  Register(gr::XE_GPU_REG_RB_STENCILREFMASK), color_info, Register(gr::XE_GPU_REG_RB_SURFACE_INFO),
                  fetch[1] & 0x3F, (fetch[2] & 0x1FFF) + 1, ((fetch[2] >> 13) & 0x1FFF) + 1,
                  fetch[1] & 0xFFFFF000u, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5], draws_);
    }
    static std::unordered_set<uint64_t> programs_seen;
    auto dump = [&](bool vertex, uint64_t hash, const std::vector<uint32_t>& words) {
      const size_t programs_max = size_t(REXCVAR_GET(masseffect_diag_missing_shader_programs_max));
      if ((programs_max && programs_seen.size() >= programs_max) ||
          !programs_seen.insert(hash ^ (vertex ? 1 : 0)).second) return;
      for (size_t first = 0; first < words.size(); first += 16) {
        std::string line;
        for (size_t i = first; i < words.size() && i < first + 16; ++i) line += fmt::format(" {:08X}", words[i]);
        REXLOG_INFO("[native] missing shader microcode {} {:016X} {} words [{}]:{}", vertex ? "VS" : "PS", hash,
                    words.size(), first, line);
      }
    };
    if (!draw_vs_) dump(true, current_vs_hash_, vs_microcode_);
    if (!draw_ps_ && (Register(kRegRbModeControl) & 0x7) != 5) dump(false, current_ps_hash_, ps_microcode_);
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
    identify_hashed_ = true;  // masseffect_native_load_memo: this load's hash may be kept by address
    identify_hash_ = memo_hash;
    const uint64_t memo_key = memo_hash ^ (uint64_t(type) << 63) ^ (uint64_t(microcode_.size()) << 40);
    if (REXCVAR_GET(masseffect_native_identity_memo)) {
      const auto it = identity_memo_.find(memo_key);
      if (it != identity_memo_.end()) {
        ApplyMemoHit(type, microcode_, memo_hash, it->second);
        return;
      }
    }
    ++microcode_epoch_[type == 1 ? 1 : 0];  // masseffect_native_split_lockstep
    if (type == 1) {
      ps_microcode_ = microcode_;
      NewIdentityGeneration(ps_identity_generation_, 0);
      current_ps_hash_ = memo_hash;
    } else {
      // Identity checks below must see THIS IM_LOAD, not the previous VS load.
      vs_microcode_ = microcode_;
      current_vs_hash_ = memo_hash;
      NewIdentityGeneration(vs_identity_generation_, 0);
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
      const masseffect::native::ShaderEntry* patched_entry = nullptr;
      if (!MatchesVertexShaderIdentity(entry) && shaders_.loaded()) {
        bool patched = false;
        const auto* matched = shaders_.Identify(true, microcode_, &patched);
        entry = PreferVertexShaderCandidate(entry, matched, "identify-library");
        if (patched && entry == matched) patched_entry = matched;
      }
      if (by_address && (!entry || (!MatchesVertexShaderIdentity(entry) &&
                                   MatchesVertexShaderIdentity(by_address))) &&
          by_address->microcode.size() == microcode_.size() &&
          masseffect::native::FetchCoherent(*by_address, microcode_)) {
        entry = PreferVertexShaderCandidate(entry, by_address, "identify-address");
      }
      if (entry == by_address && entry) ++shaders_by_address_;
      else if (entry) ++shaders_by_code_;
      // masseffect_native_vs_identify_patched: remember the programs only the second-stage lookup found, so the
      // draws they draw are counted (identity memo hits take the flag from this set, ApplyMemoHit).
      vs_patched_ = entry && entry == patched_entry;
      if (vs_patched_ && vs_patched_hashes_.size() < 4096) vs_patched_hashes_.insert(memo_hash);
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
      request.edram_bounds_rects[k] = b;
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
  // A draw inside an occlusion query that cannot change any pixel: no color writes (depth-only mode, or every
  // render target masked off), no depth writes and no stencil test (a stencil test may write stencil).
  bool QueryDrawInert() const {
    const uint32_t mode = Register(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7;
    const bool color = mode == uint32_t(xenos::EdramMode::kColorDepth) &&
                       (Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK) & 0xFFFF) != 0;
    const uint32_t dc = Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL);
    const bool depth_write = (dc & 0x2) && (dc & 0x4);
    const bool stencil = dc & 0x1;
    return (mode == uint32_t(xenos::EdramMode::kColorDepth) || mode == 5) && !color && !depth_write && !stencil;
  }

  // Query mode 3: folds this draw into the open query's identity. The identity is the content of the position
  // stream (UE3 draws its query boxes from world-space corners: stable for a static primitive whatever the
  // camera and whichever pooled query object carries it), the draw initiator, the vertex shader and the target.
  // A draw whose position fetch cannot be read leaves the query without identity ("visible").
  void QuerySignDraw() {
    if (!query_sign_ok_) return;
    query_sign_ok_ = false;
    const masseffect::native::ShaderEntry* vs = draw_vs_;
    if (!vs || vs->elements.empty()) return;
    const masseffect::native::ElementVertex* position = &vs->elements.front();
    for (const auto& element : vs->elements) {
      if (element.usage == 0) {  // D3DDECLUSAGE_POSITION
        position = &element;
        break;
      }
    }
    const size_t q = size_t(position->instruction) * 3;
    if (q + 2 >= vs_microcode_.size()) return;
    const uint32_t d0 = vs_microcode_[q], d1 = vs_microcode_[q + 1];
    if ((d0 & 0x1F) != 0 || ((d1 >> 30) & 0x1)) return;  // not a full vertex fetch (mini fetches are skipped)
    const uint32_t slot = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
    if (slot >= 96) return;
    const uint32_t f0 = Register(kRegFetchFirst + slot * 2), f1 = Register(kRegFetchFirst + slot * 2 + 1);
    if ((f0 & 0x3) != 3) return;  // FetchConstantType::kVertex
    const uint32_t base = f0 & 0x1FFFFFFCu;
    const uint32_t bytes = std::min<uint32_t>(((f1 >> 2) & 0xFFFFFF) * 4, kQuerySignBytes);
    if (!base || !bytes || uint64_t(base) + bytes > 0x20000000ull) return;
    const uint8_t* data = memory_ ? memory_->TranslatePhysical(base) : nullptr;
    if (!data) return;
    const uint64_t vertices = XXH3_64bits(data, bytes);
    const uint64_t state[6] = {query_signature_,
                               vertices,
                               Register(kRegVgtDrawInitiator),
                               vs->fingerprint,
                               Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                               Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO)};
    query_signature_ = XXH3_64bits(state, sizeof(state));
    ++query_signed_;
    query_sign_ok_ = true;
    if (query_sign_logged_ < 16) {
      ++query_sign_logged_;
      REXLOG_INFO("[native] query box draw (mode 3): position fetch slot {} at {:08X}, {} bytes hashed, initiator "
                  "{:08X}, VS n{}, content {:016X}",
                  slot, base, bytes, Register(kRegVgtDrawInitiator), vs->number, vertices);
    }
  }

  // EVENT_WRITE_ZPD of a D3D9 occlusion query (masseffect_native_query_mode).
  void ZpdQuery(uint32_t address, bool end) {
    uint8_t* guest = memory_->TranslatePhysical(address);
    if (!end) {
      // Begin structure: zero (the result is end - begin). A result still pending for the end structure of
      // the same query object (begin = end + 0x20) must not land on the new issue.
      std::memset(guest, 0, sizeof(xenos::xe_gpu_depth_sample_counts));
      if (targets_) targets_->QueryForget(address - 0x20);
      if (query_open_) ++query_unended_;
      query_open_ = true;
      query_open_begin_ = address;
      query_draws_ = 0;
      query_signature_ = 0x6F63636C75736E31ull;  // seed
      query_signed_ = 0;
      query_sign_ok_ = true;
      ++query_begun_;
      if (query_mode_ >= 2 && targets_) targets_->QueryBegin();
      return;
    }
    if (!query_open_) ++query_unbegun_;
    query_open_ = false;
    query_open_begin_ = 0;
    ++query_ended_;
    query_draws_total_ += query_draws_;
    if (targets_) targets_->QueryForget(address);
    if (query_mode_ == 1) {
      masseffect::native::WriteOcclusionCounts(guest, 0);
      return;
    }
    if (query_mode_ == 3) {
      // Latency-1: answered now from the history of the same identity; the GPU measures this issue for the next.
      uint64_t content = 0, key = 0;
      if (query_sign_ok_ && query_draws_ && query_signed_ == query_draws_) {
        content = query_signature_ | 1;  // 0 means "no identity"
        if (query_history_key_ == 0) {
          const uint64_t mixed[2] = {content, address};
          key = XXH3_64bits(mixed, sizeof(mixed)) | 1;
        } else if (query_history_key_ == 1) {
          key = content;
        }
      }
      if (query_history_key_ == 2) key = (uint64_t(1) << 32) | address;  // address only, even without content
      const uint32_t answer =
          targets_ ? targets_->QueryEndLatent(address, key, content, query_draws_, query_scale_, kQueryVisible,
                                              query_max_age_, query_hidden_after_)
                   : (query_draws_ ? kQueryVisible : 0u);
      masseffect::native::WriteOcclusionCounts(guest, answer);
      return;
    }
    if (query_mode_ == 2 && targets_ && targets_->QueryEnd(address, query_draws_, query_scale_, kQueryVisible)) {
      return;  // the sentinel stays until the GPU result (or the timeout) is written
    }
    if (query_mode_ == 2) ++query_fallbacks_now_;
    masseffect::native::WriteOcclusionCounts(guest, kQueryVisible);  // SDK fallback: 1000 samples
  }

  void Draw() {
    if (query_open_ && query_mode_ < 2 && query_skip_boxes_ && QueryDrawInert()) {
      ++query_boxes_skipped_;
      return;
    }
    if (query_open_ && query_logged_draws_ < 24) {
      ++query_logged_draws_;
      REXLOG_INFO("[native] draw inside an occlusion query: mode control {} color mask {:04X} depth control "
                  "{:08X} surface {:08X} depth info {:08X} inert {}",
                  Register(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7,
                  Register(rex::graphics::XE_GPU_REG_RB_COLOR_MASK) & 0xFFFF,
                  Register(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL),
                  Register(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO),
                  Register(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO), QueryDrawInert());
    }
    if (!MatchesVertexShaderIdentity(draw_vs_) && MatchesVertexShaderIdentity(vs_))
      draw_vs_ = PreferVertexShaderCandidate(draw_vs_, vs_, "draw-final-ring-preference");
    if (draw_vs_ && !AcceptVertexShaderIdentity(draw_vs_, "draw-final")) {
      // No unchecked fallback: the last loaded variant is the only authority.
      draw_vs_ = vs_ && AcceptVertexShaderIdentity(vs_, "draw-final-ring") ? vs_ : nullptr;
      if (!draw_vs_) { ++draws_unidentified_; return; }
    }
    if (query_open_ && query_mode_ == 3) QuerySignDraw();
    if (!EnsureTargets()) return;
    if (vs_patched_ && draw_vs_ == vs_) ++draws_vs_patched_;
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
    request.occlusion_query = query_open_ && query_mode_ >= 2;
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
    if (!BackDraw(request)) {
      if (++native_draw_failures_ <= 8)
        REXLOG_WARN("[native] draw rejected before completion; visual conformance is not established");
    }
  }

  void Copy() {
    if (!EnsureTargets()) return;
    namespace g = rex::graphics;
    // The back's registers (masseffect_native_split_lockstep: its mirror; otherwise the live file).
    const uint32_t* back = BackRegisters("copy");
    const auto Register = [back](uint32_t index) { return index < kRegisterCount ? back[index] : 0u; };
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
    const uint32_t* back = BackRegisters("present");  // masseffect_native_split_lockstep: the back's mirror
    for (uint32_t i = 0; i < 6; ++i) {
      texture.dword[i] = back[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i];
    }
    if (targets_->Present(presenter_.get(), texture, swap_width_, swap_height_)) {
      ++presented_;
    }
  }

  // --- Front/back boundary (docs/multithread-translation.md, step 1: lockstep on the ring thread) ---
  // Brings the back's mirror up to the front: MMIO registers written by game threads since the last sync and the
  // stages' microcode (if it changed) are journaled, then the journal is applied in order.
  // Start-up (and any resync): the mirror takes the full live register file and both stages' microcode; MMIO
  // writes flagged before this point are covered by the copy (the epoch is read first).
  void SplitSeed() {
    mmio_seen_epoch_ = mmio_epoch_.load(std::memory_order_acquire);
    journal_.Clear();
    mirror_.Seed(registers_.data(), registers_.size(), vs_microcode_, ps_microcode_);
    microcode_sent_ = microcode_epoch_;
    REXLOG_INFO("[native] ring split: back mirror seeded ({} registers, VS {} words, PS {} words)", registers_.size(),
                vs_microcode_.size(), ps_microcode_.size());
  }

  void SplitSync() {
    namespace partition = me::native::ring_partition;
    // CP_RB_WPTR is written by the game's MMIO kick (not flagged: one per kick); journal it when it moved.
    me::native::ring_split::JournalIfChanged(journal_, mirror_, registers_.data(), kRegCpRbWptr);
    const uint32_t epoch = mmio_epoch_.load(std::memory_order_acquire);
    if (epoch != mmio_seen_epoch_) {
      mmio_seen_epoch_ = epoch;
      for (uint32_t r = 0; r < kRegisterCount; ++r)
        if (mmio_touched_[r].load(std::memory_order_relaxed)) journal_.Reg(r, registers_[r]);
      ++split_mmio_syncs_;
    }
    if (microcode_epoch_[0] != microcode_sent_[0]) {
      microcode_sent_[0] = microcode_epoch_[0];
      journal_.Microcode(0, vs_microcode_.data(), uint32_t(vs_microcode_.size()));
    }
    if (microcode_epoch_[1] != microcode_sent_[1]) {
      microcode_sent_[1] = microcode_epoch_[1];
      journal_.Microcode(1, ps_microcode_.data(), uint32_t(ps_microcode_.size()));
    }
    if (journal_.empty()) return;
    const uint64_t t0 = partition::Now();
    split_journal_words_ += journal_.size();
    const bool ok = mirror_.Apply(journal_);
    journal_.Clear();
    split_apply_ticks_ += partition::Now() - t0;
    ++split_syncs_;
    if (!ok) SplitOff("malformed journal record", -1, 0, 0);
  }

  // The register file the back reads: the mirror (after a sync and, on schedule, a bit-exact check) or the live one.
  const uint32_t* BackRegisters(const char* what) {
    if (!split_on_) return registers_.data();
    SplitSync();
    if (split_on_ && split_verify_.Next()) SplitVerify(what);
    return split_on_ ? mirror_.registers() : registers_.data();
  }

  bool BackDraw(masseffect::native::SubmissionDraw& request) {
    if (!split_on_) return targets_->Draw(request);
    const uint32_t* registers = BackRegisters("draw");
    if (split_on_) {
      request.registers = registers;
      request.vs_microcode = mirror_.microcode(0);
      request.ps_microcode = mirror_.microcode(1);
    }
    return targets_->Draw(request);
  }

  void SplitVerify(const char* what) {
    namespace rs = me::native::ring_split;
    namespace partition = me::native::ring_partition;
    const uint64_t t0 = partition::Now();
    ++split_verified_;
    uint32_t* mirror = mirror_.registers_mutable();
    int64_t first;
    while ((first = rs::FirstDifference(mirror, registers_.data(), kRegisterCount)) >= 0) {
      const uint32_t r = uint32_t(first);
      if (mmio_touched_[r].load(std::memory_order_relaxed)) {
        // A game thread wrote it through MMIO after the sync (an existing race with the ring, not a journal gap).
        mirror[r] = registers_[r];
        ++split_mmio_races_;
        continue;
      }
      SplitOff(what, first, registers_[r], mirror[r]);
      break;
    }
    if (split_on_ && (mirror_.microcode(0) != vs_microcode_ || mirror_.microcode(1) != ps_microcode_))
      SplitOff(mirror_.microcode(0) != vs_microcode_ ? "VS microcode" : "PS microcode", -1, 0, 0);
    split_verify_ticks_ += partition::Now() - t0;
  }

  void SplitOff(const char* what, int64_t reg, uint32_t front, uint32_t back) {
    const uint32_t differ = reg >= 0 ? me::native::ring_split::CountDifferences(
                                           mirror_.registers(), registers_.data(), kRegisterCount, 1u << 20) : 0;
    REXLOG_ERROR("[native] DIFFERENCE: ring split journal ({}: register {:04X} front {:08X} back {:08X}, {} registers "
                 "differ; draw {}, packet {:08X}, verified back operations {}); the back reads the live registers "
                 "from now on", what, reg >= 0 ? uint32_t(reg) : 0xFFFFu, front, back, differ, draws_, packet_address_,
                 split_verified_);
    split_on_ = false;
    split_mmio_.store(false, std::memory_order_release);
    journal_.Clear();
  }

  void ReportSplit(double secs, uint64_t ring_draws, uint64_t swaps) {
    if (!split_on_) {  // never on, or one last line after a DIFFERENCE
      if (!mmio_touched_ || split_reported_off_) return;
      split_reported_off_ = true;
    }
    const double ms_per_tick = 1000.0 / me::native::ring_partition::TicksPerSecond();
    const uint64_t records = journal_.records() - split_reported_records_;
    const uint64_t reg_words = journal_.register_words() - split_reported_reg_words_;
    const uint64_t ucode_words = journal_.microcode_words() - split_reported_ucode_words_;
    const double apply_ms = double(split_apply_ticks_) * ms_per_tick, verify_ms = double(split_verify_ticks_) * ms_per_tick;
    REXLOG_INFO("[native] ring split (lockstep, {}) {:.1f} s, {} Swaps, {} ring draws: {} syncs, {} records, {} register "
                "words ({:.1f} per ring draw), {} microcode words, {} journal words; apply {:.2f} ms ({:.2f} us per "
                "ring draw, {:.2f} ms per Swap); verified {} back operations (cumulative), {:.2f} ms; MMIO registers "
                "{} distinct, {} re-journals, {} races",
                split_on_ ? "on" : "OFF after a DIFFERENCE", secs, swaps, ring_draws, split_syncs_, records, reg_words,
                ring_draws ? double(reg_words) / double(ring_draws) : 0.0, ucode_words, split_journal_words_, apply_ms,
                ring_draws ? apply_ms * 1000.0 / double(ring_draws) : 0.0, swaps ? apply_ms / double(swaps) : 0.0,
                split_verified_, verify_ms, mmio_distinct_.load(std::memory_order_relaxed), split_mmio_syncs_,
                split_mmio_races_);
    split_reported_records_ = journal_.records();
    split_reported_reg_words_ = journal_.register_words();
    split_reported_ucode_words_ = journal_.microcode_words();
    split_syncs_ = split_journal_words_ = split_apply_ticks_ = split_verify_ticks_ = 0;
  }

  // masseffect_native_effects_stats (step 0): guest-visible effects of this interval, per Swap.
  void ReportEffects(double secs, uint64_t swaps) {
    if (!effects_on_) return;
    namespace rs = me::native::ring_split;
    const double per = double(std::max<uint64_t>(1, swaps));
    std::string kinds, gaps;
    for (uint32_t k = 0; k < rs::kEffectCount; ++k)
      kinds += fmt::format("{}{} {} ({:.1f}/Swap)", k ? ", " : "", rs::kEffectNames[k], effects_.effects[k],
                           double(effects_.effects[k]) / per);
    for (uint32_t b = 0; b < rs::kGapBuckets; ++b)
      gaps += fmt::format("{}{}: {}", b ? ", " : "", rs::kGapNames[b], effects_.gaps[b]);
    const uint64_t total = effects_.Total();
    REXLOG_INFO("[native] guest-visible effects ({:.1f} s, {} Swaps): {} total ({:.1f}/Swap), {} after back work "
                "= step-2 barriers ({:.1f}/Swap); {}; ring draws between consecutive effects: {}; max {}",
                secs, swaps, total, double(total) / per, effects_.barriers, double(effects_.barriers) / per, kinds,
                gaps, effects_.max_gap);
    effects_.ResetInterval();
  }

  // masseffect_native_coherence_stats (me_frame_coherence.h): keys of the draw that just ended.
  void CoherenceEndDraw() {
    namespace co = me::native::coherence;
    co::Inputs in;
    in.registers = registers_.data();
    in.vs = draw_vs_;
    in.ps = draw_ps_;
    in.vs_hash = current_vs_hash_;
    in.ps_hash = current_ps_hash_;
    in.vs_constant_words = draw_vs_ ? draw_vs_->constants_bytes / 4 : 0;
    in.ps_constant_words = draw_ps_ ? draw_ps_->constants_bytes / 4 : 0;
    in.generation_constants_vs = gen_constants_vs_;
    in.generation_constants_ps = gen_constants_ps_;
    in.initiator = Register(kRegVgtDrawInitiator);
    in.dma_base = Register(kRegVgtDmaBase);
    in.dma_size = Register(kRegVgtDmaSize);
    uint8_t slots[32];
    uint32_t n = 0;
    for (const masseffect::native::ShaderEntry* entry : {draw_vs_, draw_ps_})
      if (entry)
        for (const auto& sampler : entry->samplers)
          if (n < 32) slots[n++] = uint8_t(sampler.register_value & 31);
    in.texture_slots = slots;
    in.texture_slot_count = n;
    co::EndDraw(in);
  }
  void CoherenceEndFrame() {
    std::string line1, line2;
    if (me::native::coherence::EndFrame(line1, line2)) {
      REXLOG_INFO("{}", line1);
      REXLOG_INFO("{}", line2);
    }
  }

  // masseffect_native_ring_partition: where the ring thread's time went since the previous report.
  void ReportRingPartition(double secs, uint64_t ring_draws, uint64_t swaps, uint64_t vulkan_draws) {
    namespace partition = me::native::ring_partition;
    if (!partition::Active()) return;
    std::array<uint64_t, partition::kCount> ticks{}, entries{};
    partition::Take(ticks, entries);
    const double ms_per_tick = 1000.0 / partition::TicksPerSecond();
    uint64_t busy = 0;
    for (uint32_t k = 0; k < partition::kCount; ++k)
      if (k != partition::kWait) busy += ticks[k];
    const double per_draw = ring_draws ? 1000.0 / double(ring_draws) : 0.0;  // ms -> us per ring draw
    std::string parts;
    for (uint32_t k = 0; k < partition::kCount; ++k) {
      const double ms = double(ticks[k]) * ms_per_tick;
      parts += fmt::format(" | {} {:.1f} ms ({:.2f} us/draw, {} entries)", partition::kNames[k], ms, ms * per_draw,
                           entries[k]);
    }
    REXLOG_INFO("[native] ring partition ({:.1f} s, {} Swaps, {} ring draws, {} Vulkan draws): busy {:.1f} ms = "
                "{:.1f} % of the interval, {:.2f} us per ring draw{}",
                secs, swaps, ring_draws, vulkan_draws, double(busy) * ms_per_tick,
                secs > 0 ? double(busy) * ms_per_tick / (secs * 10.0) : 0.0, double(busy) * ms_per_tick * per_draw,
                parts);
  }
  uint64_t partition_drawn_ = 0;

  // masseffect_native_waitregmem_stats: per condition (memory/register, address, reference, mask).
  struct WaitRegMemStat {
    uint64_t calls = 0, waited = 0, ns = 0, max_ns = 0;
  };
  std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, WaitRegMemStat> waitregmem_stats_;
  void NoteWaitRegMem(uint32_t info, uint32_t poll, uint32_t ref, uint32_t mask, bool waited, uint64_t ns) {
    auto& e = waitregmem_stats_[{info & 0x17u, poll, ref, mask}];
    ++e.calls;
    if (waited) ++e.waited;
    e.ns += ns;
    if (ns > e.max_ns) e.max_ns = ns;
  }
  void ReportWaitRegMem() {
    if (waitregmem_stats_.empty()) return;
    std::vector<std::pair<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, WaitRegMemStat>> v(
        waitregmem_stats_.begin(), waitregmem_stats_.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    std::string text;
    for (size_t i = 0; i < v.size() && i < 8; ++i) {
      const auto& [k, e] = v[i];
      text += fmt::format(" [{} {:08X} fn {} ref {:08X} mask {:08X}: {} calls, {} waited, {:.1f} ms, max {:.2f} ms]",
                          (std::get<0>(k) & 0x10) ? "mem" : "reg", std::get<1>(k), std::get<0>(k) & 7, std::get<2>(k),
                          std::get<3>(k), e.calls, e.waited, e.ns / 1e6, e.max_ns / 1e6);
    }
    REXLOG_INFO("[native] WAIT_REG_MEM since last report:{}; early VBlanks (cumulative) {}", text, vblanks_early_.load());
    waitregmem_stats_.clear();
  }

  void Report(bool force) {
    const auto now = Clock::now();
    if (!force && now - last_report_ < std::chrono::seconds(10)) return;
    ReportWaitRegMem();
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
            "clears", "scene_no_z", "gap",         "edram_import", "edram_export", "edram_alias", "edram_import9",
            "occlusion_depth"};
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
    if (load_memo_) {
      REXLOG_INFO("[native] shader load memo: {} memo hits without swap and XXH3; table {} lookups, {} equal, {} "
                  "with changed words (cumulative)", load_memo_hits_, load_memo_->lookups(), load_memo_->hits(),
                  load_memo_->changed());
    }
    load_memo_hits_ = 0;
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
    if (shaders_.loaded()) {
      const masseffect::native::StatsShaders shader_stats = shaders_.Stats();
      if (shader_stats.patched || draws_vs_patched_) {
        REXLOG_INFO("[native] VS patched-fetch lookup: {} draws rescued (drawn with a VS only that lookup found; "
                    "cumulative); {} distinct programs found ({} with same-register fetches reordered, {} with "
                    "several candidates)",
                    draws_vs_patched_, shader_stats.patched, shader_stats.patched_permuted,
                    shader_stats.patched_ambiguous);
      }
    }
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
    ReportRingPartition(secs, draws_ - reported_draws_, swaps_ - reported_swaps_, st.drawn - partition_drawn_);
    ReportEffects(secs, swaps_ - reported_swaps_);
    ReportSplit(secs, draws_ - reported_draws_, swaps_ - reported_swaps_);
    partition_drawn_ = st.drawn;
    if (query_begun_ != reported_query_begun_ || query_ended_ != reported_query_ended_) {
      const uint64_t swaps = std::max<uint64_t>(1, swaps_ - reported_query_swaps_);
      const uint64_t ended = query_ended_ - reported_query_ended_;
      std::string real;
      if (query_mode_ == 3 && targets_) {
        const auto q = targets_->StatsOfQueries();
        const auto& o = reported_queries_;
        const uint64_t resolved = q.resolved - o.resolved;
        real = fmt::format("; latency-1: {} answered from history ({} hidden), {} unknown, {} stale; {} measured, {} "
                           "not measured; {} folded ({} with 0 samples, avg {:.0f} samples, avg age {:.2f} frames / "
                           "{:.2f} ms), {} read failures; table {} entries; box content at another address {}; {} "
                           "empty, {} Vulkan queries",
                           q.answered_history - o.answered_history, q.answered_hidden - o.answered_hidden,
                           q.answered_unknown - o.answered_unknown, q.answered_stale - o.answered_stale,
                           q.latent_measured - o.latent_measured, q.latent_unmeasured - o.latent_unmeasured, resolved,
                           q.resolved_zero - o.resolved_zero,
                           resolved ? double(q.samples - o.samples) / double(resolved) : 0.0,
                           resolved ? double(q.latency_frames - o.latency_frames) / double(resolved) : 0.0,
                           resolved ? double(q.latency_us - o.latency_us) / 1000.0 / double(resolved) : 0.0,
                           q.fallback_read - o.fallback_read, q.history_size, q.content_moved - o.content_moved,
                           q.empty - o.empty, q.vulkan_queries - o.vulkan_queries);
        reported_queries_ = q;
      }
      if (query_mode_ == 2 && targets_) {
        const auto q = targets_->StatsOfQueries();
        const auto& o = reported_queries_;
        const uint64_t resolved = q.resolved - o.resolved;
        real = fmt::format("; real: {} resolved ({} with 0 samples, avg {:.0f} samples), latency avg {:.2f} frames / "
                           "{:.2f} ms; fallbacks to visible: {} unmeasured, {} split, {} timeout, {} read, {} no "
                           "targets; {} empty, {} superseded, {} Vulkan queries, {} early submissions",
                           resolved, q.resolved_zero - o.resolved_zero,
                           resolved ? double(q.samples - o.samples) / double(resolved) : 0.0,
                           resolved ? double(q.latency_frames - o.latency_frames) / double(resolved) : 0.0,
                           resolved ? double(q.latency_us - o.latency_us) / 1000.0 / double(resolved) : 0.0,
                           q.fallback_unmeasured - o.fallback_unmeasured, q.fallback_split - o.fallback_split,
                           q.fallback_timeout - o.fallback_timeout, q.fallback_read - o.fallback_read,
                           (query_fallbacks_now_ - reported_query_fallbacks_now_) -
                               ((q.fallback_unmeasured - o.fallback_unmeasured) + (q.fallback_split - o.fallback_split)),
                           q.empty - o.empty, q.superseded - o.superseded, q.vulkan_queries - o.vulkan_queries,
                           q.flushes - o.flushes);
        reported_queries_ = q;
      }
      REXLOG_INFO("[native] occlusion queries (mode {}): {} begun, {} ended ({:.1f} per Swap), {} draws inside, {} "
                  "boxes skipped; unpaired: {} begins without end, {} ends without begin (cumulative); {} ends "
                  "whose sentinel was already erased{}{}",
                  query_mode_, query_begun_ - reported_query_begun_, ended, double(ended) / double(swaps),
                  query_draws_total_ - reported_query_draws_, query_boxes_skipped_ - reported_query_boxes_,
                  query_unended_, query_unbegun_, query_end_by_address_ - reported_query_end_by_address_,
                  query_pair_by_address_ ? " (paired by address)" : " (pairing off: taken for begins and zeroed)", real);
      reported_query_end_by_address_ = query_end_by_address_;
      reported_query_begun_ = query_begun_;
      reported_query_ended_ = query_ended_;
      reported_query_draws_ = query_draws_total_;
      reported_query_boxes_ = query_boxes_skipped_;
      reported_query_fallbacks_now_ = query_fallbacks_now_;
      reported_query_swaps_ = swaps_;
    }
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
  // D3D9 occlusion queries (masseffect_native_query_mode, docs/occlusion-queries.md).
  static constexpr uint32_t kQueryVisible = 1000;  // the SDK's fake count (query_occlusion_fake_sample_count)
  int32_t query_mode_ = 0;
  bool query_skip_boxes_ = false;
  int32_t query_flush_us_ = 500;
  uint32_t query_timeout_us_ = 50000;
  // Query mode 3 (latency-1): history rules, identity of the open query, fence poll pacing.
  static constexpr uint32_t kQuerySignBytes = 4096;  // position data hashed per draw (a box batch is ~1 KB)
  uint32_t query_max_age_ = 4;
  uint32_t query_hidden_after_ = 2;
  int32_t query_history_key_ = 0;
  uint64_t query_signature_ = 0;
  uint32_t query_signed_ = 0;
  bool query_sign_ok_ = false;
  uint32_t query_sign_logged_ = 0;
  std::chrono::steady_clock::time_point query_serviced_{};
  double query_scale_ = 1.0;  // internal resolution -> 1280x720 sample counts
  bool query_open_ = false;
  bool query_pair_by_address_ = true;
  uint32_t query_open_begin_ = 0;  // RB_SAMPLE_COUNT_ADDR of the open query's BEGIN (its end structure + 0x20)
  uint64_t query_end_by_address_ = 0, reported_query_end_by_address_ = 0;
  uint32_t query_draws_ = 0;
  uint32_t query_logged_draws_ = 0;
  uint64_t query_begun_ = 0, query_ended_ = 0, query_unended_ = 0, query_unbegun_ = 0, query_draws_total_ = 0,
           query_boxes_skipped_ = 0, query_fallbacks_now_ = 0;
  uint64_t reported_query_begun_ = 0, reported_query_ended_ = 0, reported_query_draws_ = 0,
           reported_query_boxes_ = 0, reported_query_fallbacks_now_ = 0, reported_query_swaps_ = 0;
  masseffect::native::StatsQueries reported_queries_{};
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
  // masseffect_native_vs_identify_patched: the current VS was found only by the second-stage lookup; the hashes of
  // such programs (for identity memo hits); draws drawn with them.
  bool vs_patched_ = false;
  std::unordered_set<uint64_t> vs_patched_hashes_;
  uint64_t draws_vs_patched_ = 0;
  uint64_t vs_identity_generation_ = 1;
  uint64_t ps_identity_generation_ = 1;
  std::unordered_map<const masseffect::native::ShaderEntry*, std::pair<uint64_t, bool>> vs_identity_cache_;
  std::array<IdentityCell, kIdentityCells> vs_cells_{}, ps_cells_{}, fc_cells_{};
  // Ring CPU switches (masseffect_native_flat_identity, _fast_pair, _raw_microcode, _pm4_fast) and the
  // remaining self-check uses of each.
  bool identity_flat_ = false, pairing_fast_ = false, raw_microcode_ = false, pm4_fast_ = false;
  uint32_t check_identity_ = 0, check_pairing_ = 0, check_raw_ = 0, check_pm4_ = 0, check_objects_ = 0;
  std::vector<uint32_t> raw_vs_, raw_ps_;  // the guest words of the last IM_LOAD of each stage (raw_microcode_)
  // masseffect_native_load_memo (me_shader_load_memo.h); null when off or after a self-check DIFFERENCE.
  std::unique_ptr<ShaderLoadMemo> load_memo_;
  uint32_t check_load_memo_ = 0;
  bool load_memo_generation_ = false;  // masseffect_native_load_memo_generation (needs the load memo)
  uint64_t identity_generation_counter_ = 1;
  uint64_t load_memo_hits_ = 0;
  bool identify_hashed_ = false;  // IdentifyShader computed identify_hash_ (XXH3 of microcode_) for this load
  uint64_t identify_hash_ = 0;
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
  // masseffect_vblank_adaptive
  std::atomic<bool> adaptive_vblank_{false};
  std::atomic<uint64_t> last_swap_ns_{0}, vblank_interval_ns_{0};
  std::atomic<bool> vblank_early_{false};
  std::mutex vblank_mutex_;
  std::condition_variable vblank_cv_;
  uint64_t early_requested_swap_ = UINT64_MAX;
  std::atomic<uint64_t> vblanks_early_{0};
  uint64_t opcodes_[128] = {};
  uint64_t opcode_words_[128] = {}, type0_packets_ = 0, type0_words_ = 0, type0_single_ = 0;  // PM4 mix report
  std::atomic<uint64_t> interrupts_{0};
  uint64_t reported_swaps_ = 0, reported_draws_ = 0, reported_copies_ = 0, reported_packets_ = 0,
           reported_interrupts_ = 0;
  Clock::time_point start_, last_report_;
  // masseffect_native_effects_stats (step 0) and masseffect_native_split_lockstep (step 1), me_ring_split.h.
  bool effects_on_ = false;
  me::native::ring_split::EffectStats effects_;
  bool split_on_ = false, split_reported_off_ = false;
  me::native::ring_split::Journal journal_;
  me::native::ring_split::Mirror mirror_;
  me::native::ring_split::VerifySchedule split_verify_;
  std::array<uint64_t, 2> microcode_epoch_{}, microcode_sent_{~0ull, ~0ull};
  std::atomic<bool> split_mmio_{false};
  std::unique_ptr<std::atomic<uint8_t>[]> mmio_touched_;  // registers written through MMIO by game threads
  std::atomic<uint32_t> mmio_epoch_{0}, mmio_distinct_{0};
  uint32_t mmio_seen_epoch_ = 0;
  uint64_t split_syncs_ = 0, split_journal_words_ = 0, split_apply_ticks_ = 0, split_verify_ticks_ = 0,
           split_verified_ = 0, split_mmio_syncs_ = 0, split_mmio_races_ = 0;
  uint64_t split_reported_records_ = 0, split_reported_reg_words_ = 0, split_reported_ucode_words_ = 0;
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
