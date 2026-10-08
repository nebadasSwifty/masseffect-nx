// Exact pixel shader rewrites around descriptors and clamps: the settings (docs/scene-shader-cost.md, "Descriptor and
// clamp rewrites"). Included at file scope by masseffect_native_draws.cpp. The SPIR-V transforms are in
// me_ps_descriptors_spirv.h, the renderer side in masseffect_ps_descriptors_members.inc (inside DrawsVulkanImpl).
//
// All three need masseffect_native_constants_ubo (the shaders then read the shared block from set 4). Each one marks
// its pipelines with a bit of PipelineKey::signs_heaps (bits 24-26, kKeyPs*), so they never mix with the usual ones.

/*
 * A. The two clamp bounds of shader_common.h (FLT_MIN = 0xFF7FFFFF, FLT_MAX = 0x7F7FFFFF) read from shared words
 * 122/123, which the renderer always writes with those bit patterns, instead of being 32-bit immediates that NAK
 * materialises with a `mov` before every clamp. Same operations on the same values. -3.4 % NAK instructions over the
 * library (offline estimate ~0.16 ms/frame at 1280x720).
 */
REXCVAR_DEFINE_BOOL(masseffect_native_ps_flt_bounds_ubo, false, "MASSEFFECT",
                    "Native renderer: pixel shaders read the clamp bounds (+-FLT_MAX) from the shared constants "
                    "instead of 32-bit immediates. Exact (same image)");
/*
 * B. A third descriptor heap of COMBINED_IMAGE_SAMPLER descriptors (set 5), one per (2D texture slot, sampler slot)
 * pair a draw uses, mirrored whenever the 2D slot is rewritten. Pixel shaders whose 2D descriptor words provably only
 * index the 2D heap fetch the combined descriptor (one index load, one handle) instead of a texture and a sampler
 * descriptor; the renderer writes the combined index into the 2D descriptor word. Same view, same sampler: same
 * image. -7 NAK instructions per 2D fetch (with A and C: 252 -> 202 on average, ~0.45 ms/frame at 1280 offline).
 * Read at start-up for the pipeline layout (set 5 exists only when this is on at start-up); afterwards it can be
 * switched off and on again at run time (once per frame).
 */
REXCVAR_DEFINE_BOOL(masseffect_native_ps_combined_heap, false, "MASSEFFECT",
                    "Native renderer: 2D texture fetches of the pixel shaders use one combined image+sampler "
                    "descriptor (set 5) instead of separate texture and sampler descriptors. Exact (same image). "
                    "Needs a restart to create the heap");
/*
 * C. With masseffect_native_fold_texture_signs folding every sampled register of the pixel shader, nothing reads the
 * sign byte (bits 24-31) of those descriptor words any more: the renderer writes them with the byte cleared and the
 * `& 0xFFFFFF` of every fetch is removed from the module. ~0.07 ms/frame at 1280 offline.
 */
REXCVAR_DEFINE_BOOL(masseffect_native_ps_no_index_mask, false, "MASSEFFECT",
                    "Native renderer: with masseffect_native_fold_texture_signs, write the descriptor words without "
                    "the sign byte and drop the `& 0xFFFFFF` of the pixel shader fetches. Exact (same image)");

// Bits of PipelineKey::signs_heaps (bits 0-15: heaps of the folded signs, 16-19: their count).
constexpr uint32_t kKeyPsFltBounds = uint32_t(1) << 24;  // A applied to the pixel shader module
constexpr uint32_t kKeyPsCombined = uint32_t(1) << 25;   // B: 2D words hold combined heap indices
constexpr uint32_t kKeyPsNoMask = uint32_t(1) << 26;     // C: sign bytes of the folded registers cleared
constexpr uint32_t kKeyPsRewrites = kKeyPsFltBounds | kKeyPsCombined | kKeyPsNoMask;
