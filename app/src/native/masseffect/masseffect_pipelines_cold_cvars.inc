// Cold-start pipeline hitches: the settings (docs/cold-start-hitches.md). Included at file scope by
// masseffect_native_draws.cpp, right after the pipeline prewarm settings. The code is in
// masseffect_pipelines_cold_members.inc (inside DrawsVulkanImpl).

#include "me_packaged.h"  // ShippedFile: where masseffect_native_pipelines_shipped_list is looked up

/*
 * Specialized pipelines compiled in the background.
 *
 * masseffect_native_fold_vs_constants and masseffect_native_fold_texture_signs put values in the pipeline key and
 * fold them into the shader module. The module computes exactly what the unfolded (generic) module computes from the
 * same constants, which the draw uploads in both cases. So while the specialized pipeline is being compiled, the
 * draw can use the generic one and the image does not change: only the GPU saving of the folding is postponed.
 *
 * 0 = off (as before: the ring compiles the specialized pipeline and the frame waits for it).
 * 1 = only when the generic pipeline already exists in this session (never one extra synchronous compile).
 * 2 = always: if the generic one does not exist it is compiled on the ring (the frame waits for that one, as it
 *     would have waited for the specialized one), and every later value set of the same shader and state is free.
 * Before deferring, the specialized pipeline is first looked up in the Vulkan cache with
 * VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT (masseffect_native_pipelines_async_probe): on a warm
 * run it is a 0-2 ms cache hit and is used at once, exactly as before.
 */
REXCVAR_DEFINE_INT32(masseffect_native_pipelines_async_specialized, 0, "MASSEFFECT",
                     "Native renderer: compile pipelines specialized by folded constants (VS constants, texture "
                     "signs) on a background thread and draw with the generic pipeline (same image) until they "
                     "are ready. 0 = off, 1 = only if the generic pipeline exists, 2 = always")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(masseffect_native_pipelines_async_probe, true, "MASSEFFECT",
                    "Native renderer: with masseffect_native_pipelines_async_specialized, first ask the Vulkan "
                    "cache for the specialized pipeline (FAIL_ON_PIPELINE_COMPILE_REQUIRED) and only defer it when "
                    "it would have to be compiled. false = always defer");
REXCVAR_DEFINE_INT32(masseffect_native_pipelines_async_priority, -1, "MASSEFFECT",
                     "Native renderer: priority of the background pipeline compile thread (0x2C-0x3F; -1 = the "
                     "lowest the system accepts from 0x3F to 0x3B, like the prewarm thread)")
    .range(-1, 0x3F)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_pipelines_async_core, -1, "MASSEFFECT",
                     "Native renderer: preferred core of the background pipeline compile thread (-1 = leave it, "
                     "-2 = a core other than the ring's, 0-2 = that core)")
    .range(-2, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * Writing cache/masseffect_native_pipelines.bin (16 MB) without stalling the game.
 *
 * Measured (run/me1/restore_r, vsfold_pd_b): every save is followed by a 0.3-1.8 s frame that ends when the file
 * write ends ("pipelines saved ... in 1642-2443 ms"). One ofstream::write of 16 MB is one long SD card request,
 * and the game's own file reads (and the log) queue behind it. Written in pieces with a short pause between them,
 * other requests get the card between pieces. vkGetPipelineCacheData (two full serializations of the cache) also
 * moves from the ring to the writer thread. Only the way the cache file is written changes.
 */
REXCVAR_DEFINE_INT32(masseffect_native_pipelines_save_chunk_kb, 256, "MASSEFFECT",
                     "Native renderer: the pipelines file is written in pieces of this many KB with a pause "
                     "between them (masseffect_native_pipelines_save_pause_ms). 0 = one write, as before")
    .range(0, 65536);
REXCVAR_DEFINE_INT32(masseffect_native_pipelines_save_pause_ms, 8, "MASSEFFECT",
                     "Native renderer: pause between two pieces of the pipelines file (ms)")
    .range(0, 500);
REXCVAR_DEFINE_BOOL(masseffect_native_pipelines_save_serialize_off_ring, true, "MASSEFFECT",
                    "Native renderer: vkGetPipelineCacheData runs on the cache writer thread instead of the ring. "
                    "false = on the ring, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * A prewarm list shipped with the build (our data, not game data: pipeline keys, vertex formats and shader
 * fingerprints). It is the list part of cache/masseffect_native_pipelines.bin after a long console run, extracted by
 * tools/extract_prewarm_list.py, and lives next to the NRO (not in cache/, so a cold test keeps it; an installed NSP
 * carries it in its RomFS, and a copy in the SD data folder wins). Each edition has its own (shader fingerprints of
 * its own shader package): the installer and the NSP put the edition's file there. Its records
 * that the normal list does not have are appended to it, so the prewarm thread compiles them in a cold run too.
 */
REXCVAR_DEFINE_STRING(masseffect_native_pipelines_shipped_list, "", "MASSEFFECT",
                      "Native renderer: extra pipeline prewarm list (NFPL format) merged into the normal one at "
                      "start-up; a path, relative to the NRO folder unless absolute (installed NSP: the SD "
                      "data folder, then romfs:/), e.g. masseffect_prewarm_list.bin. Empty = none; a missing "
                      "file is a warning")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_pipelines_prewarm_threads, 1, "MASSEFFECT",
                     "Native renderer: how many threads prewarm the pipeline list (1 = as before). The extra "
                     "threads run at the same priority, on another core than the ring's when possible")
    .range(1, 3)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_pipelines_prewarm_priority, -1, "MASSEFFECT",
                     "Native renderer: priority of the prewarm threads (0x2C-0x3F). -1 = the lowest the system "
                     "accepts from 0x3F to 0x3B, as before")
    .range(-1, 0x3F)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
