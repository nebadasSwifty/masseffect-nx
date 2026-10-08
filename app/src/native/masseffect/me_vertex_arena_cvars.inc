// Cross-frame vertex arena: the settings (docs/zero-copy-vertices.md, "Cross-frame vertex arena"). Included at file
// scope by masseffect_native_draws.cpp. The logic is in me_vertex_arena.h, the renderer side in
// me_vertex_arena_members.inc (inside DrawsVulkanImpl). Both switches are off by default.
REXCVAR_DEFINE_BOOL(masseffect_native_vertex_arena_measure, false, "MASSEFFECT",
                    "Native renderer, measurement only (same image): every copied vertex range (guest address, bytes, "
                    "byte order, stride) gets a full XXH3 and the coherency stamp; its next copy is classified as "
                    "clean (no coherency event on its pages since) or dirty x content same or changed. Every 10 s: "
                    "ranges, MB, clean+same (what the arena would not copy), clean+CHANGED (writes no event declared: "
                    "must be 0 outside exempt ranges), dirty+same, dirty+changed, and the top 5 clean+CHANGED ranges. "
                    "Costs one full hash per copied range")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_vertex_arena, false, "MASSEFFECT",
                    "Native renderer: vertex ranges seen twice with the same content are copied (byte-swapped) once "
                    "into a persistent buffer and bound from there in later draws and frames while no coherency event "
                    "touched their pages since that copy (and the check of masseffect_native_vertex_arena_check "
                    "passes). Pages where a range changed without an event are exempt for the session; the first "
                    "masseffect_native_vertex_arena_verify_n hits and 1 in _verify_every are compared with a full "
                    "hash (DIFFERENCE turns the arena off). The space is reused only after the GPU finished every "
                    "frame that read it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_vertex_arena_mb, 32, "MASSEFFECT",
                     "Native renderer, masseffect_native_vertex_arena: size of the persistent vertex buffer (MB), "
                     "split in 8 segments recycled in turn (a range larger than a segment is never stored)")
    .range(8, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_vertex_arena_check, 1, "MASSEFFECT",
                     "Native renderer, masseffect_native_vertex_arena: content check of a clean hit besides the "
                     "coherency stamp. 0 = none (stamp only; the verification schedule still runs), 1 = the sampled "
                     "fingerprint of the vertex dedupe (every 8th 4 KB block plus the last; ranges up to 8 KB are read "
                     "whole; usually already computed for the dedupe), 2 = full XXH3 of the range (saves only the copy's "
                     "write into uncached memory)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_vertex_arena_min_bytes, 256, "MASSEFFECT",
                     "Native renderer, masseffect_native_vertex_arena: ranges smaller than this are always copied as "
                     "before (the lookup would cost about as much as the copy)")
    .range(4, 1 << 20)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_vertex_arena_max_undeclared, 4, "MASSEFFECT",
                     "Native renderer, masseffect_native_vertex_arena: changes with no coherency event seen on ranges "
                     "larger than 8 KB that had already been reused (where the sampled check does not read every "
                     "byte). After more than N of them the arena turns off for the session. Every such range (any "
                     "size) makes its pages exempt. 0 = off at the first one")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_vertex_arena_verify_n, 4096, "MASSEFFECT",
                     "Native renderer, masseffect_native_vertex_arena: the first N arena hits also hash the guest range "
                     "in full and compare it with the hash taken when the copy was made (DIFFERENCE turns the arena "
                     "off). 0 = none")
    .range(0, 100000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_vertex_arena_verify_every, 64, "MASSEFFECT",
                     "Native renderer, masseffect_native_vertex_arena: after the first _verify_n hits, 1 hit in N is "
                     "still checked in full. 0 = none")
    .range(0, 100000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
