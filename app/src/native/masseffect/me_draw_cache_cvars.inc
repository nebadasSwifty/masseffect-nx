// Per-component draw cache: the settings (docs/frame-coherence.md, "Per-component cache"). Included at file scope by
// masseffect_native_draws.cpp. The logic is in me_draw_cache.h, the renderer side in me_draw_cache_members.inc
// (inside DrawsVulkanImpl). Every component is off by default and checks itself against the path it replaces.
REXCVAR_DEFINE_BOOL(masseffect_native_draw_cache_textures, false, "MASSEFFECT",
                    "Native renderer (exact): the texture bindings of a draw's pixel shader are remembered by the "
                    "shader's sampler registers and their fetch words. A later draw with the same list primes the "
                    "per-register sampler cache with them when every binding is still valid by that cache's own rule "
                    "(frame horizon, generations, address buckets, checks), skipping the per-fetch table and "
                    "PrepareTexture. Checked: masseffect_native_draw_cache_verify_n / _every")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_draw_cache_pipelines, false, "MASSEFFECT",
                    "Native renderer (exact): raw pipeline key -> pipeline, kept once the pipeline is final (found in "
                    "the pipeline map). A hit skips the lookup key (canonical form, dynamic-state masking) and the "
                    "pipeline lookup. Checked: masseffect_native_draw_cache_verify_n / _every")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_native_draw_cache_indices, false, "MASSEFFECT",
                    "Native renderer (exact): 16-bit index ranges with a full fingerprint (up to 16 KB, the index "
                    "cache's) are converted once into a persistent buffer and reused across frames while address, "
                    "byte order, count and fingerprint are equal; the buffer is reused only after the GPU finished "
                    "every frame that read it. Needs masseffect_native_indices_cache; off while the sky is postponed. "
                    "Checked: masseffect_native_draw_cache_verify_n / _every")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_draw_cache_indices_mb, 8, "MASSEFFECT",
                     "Native renderer, masseffect_native_draw_cache_indices: size of the persistent index buffer (MB)")
    .range(1, 64)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_draw_cache_verify_n, 4096, "MASSEFFECT",
                     "Native renderer, draw cache: the first N hits of each component also run the path they replace "
                     "and compare the results bit for bit (DIFFERENCE turns that component off). 0 = none")
    .range(0, 100000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_native_draw_cache_verify_every, 4096, "MASSEFFECT",
                     "Native renderer, draw cache: after the first masseffect_native_draw_cache_verify_n hits, 1 hit "
                     "in N is still checked. 0 = none")
    .range(0, 100000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
