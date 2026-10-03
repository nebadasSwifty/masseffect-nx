# The native renderer

This page explains how the port draws Mass Effect with Vulkan: what the game sends to the GPU, how that is read, how the
Xbox 360's special memory ([EDRAM](glossary.md#edram)) is imitated, how shaders are found, and what each part cost in
speed. It is a condensed map, not a line-by-line description of the code. The speed work on top of it is in
[performance-history.md](performance-history.md) and [optimization-paths.md](optimization-paths.md); words you do not
know are in [glossary.md](glossary.md).

## In short

- The game keeps its own Direct3D layer, statically linked in its code. The port does not replace it. Direct3D writes a
  list of GPU commands into a ring buffer (the [PM4 ring](glossary.md#pm4-ring)); a **ring thread** reads that list and
  records the same drawing in Vulkan.
- The SDK's own GPU emulation (which translates shaders while the game runs, and watches pages of guest memory) is not
  loaded. That removed the stage 2 crash at its root: no watched memory, no emulated reads.
- Shaders are never translated while playing. A **shader library** is built beforehand, from the game's own data,
  and the renderer only looks shaders up in it.
- The Xbox 360 draws into 10 MB of EDRAM that has no equivalent on a PC or the Switch. The port keeps one Vulkan image per
  format and moves data between them when needed: **EDRAM mode 4**. Most of the performance work was making these moves
  rare and cheap.

## Why not the stock GPU emulation

In stage 2 the SDK's backend gave 1.5-2.5 fps in menus and crashed in game (see
[performance-history.md](performance-history.md#1-the-emulated-gpu-no-usable-game-stage-2)). Its design needs the CPU
to translate and replay every draw, and it needs the SDK to know when the game writes to memory the GPU read. On the
Switch watching memory costs an exception per read, and the exception path loses vector registers. A native renderer
removes both.

## The path of one frame

```
game thread (Unreal Engine 3 + the game's Direct3D)
   |  writes PM4 packets: register writes, constants, shader loads, draws, resolves, swap
   v
ring buffer in guest memory
   |
ring thread: parse PM4 ---> track GPU state ---> record draws as Vulkan commands
   |                                                  |
   |                       (optional) worker thread records the queued commands
   v                                                  v
fence / read-pointer write-back to the game      Vulkan queue ---> present
```

The ring thread understands the packet types the game uses: register writes, interrupts, swap, indirect buffers,
wait-on-register, register read-modify-write, memory writes, conditional writes, events (also with a fence value),
constant loads (several kinds), bin mask and select, and indexed draws. It writes the scratch register values to the
address the game polls and returns its read pointer, which is how the game thread knows the GPU has "finished".

How the game's Direct3D works (verified by tracing 60 functions in a 90 s run of one level):

| Piece | What it is |
|---|---|
| Direct3D | the Xbox development kit's, at addresses `0x8221F000-0x8223FFFF`; Unreal Engine 3's render hardware interface calls 54 of its entry points |
| Per frame (one level, example) | 1 swap, 279 calls of one indexed draw, 81 of another, about 1071 texture setters, 266 vertex-stream setters |
| State | the setters write no packets: they fill a mirror of the device registers; helpers called by the draws emit the shader loads and the constants |
| Draw bodies | all four write the draw packet plus context-update and event-write |

This is the same design as most Xbox 360 Direct3D layers (mirror plus flush), which is why a ring reader works.

## Shaders

### Finding which shader a draw uses

Unreal Engine 3 builds its shader objects in place from its own shader cache, so (unlike some other games) no
creation call needs a hook. At draw time the bound vertex shader is at device offset 12416 and the pixel shader at
12412; word 0 of the object is its type (6 vertex, 7 pixel); a copy of the original container sits inside the object
(pixel at +0x28, vertex at +0x368).

Two things make identification hard:

1. **Direct3D rewrites vertex shaders** to match the vertex declaration, in place, at the first bind. So the microcode the
   ring sees is a variant of the original. The renderer identifies a vertex shader by the loaded microcode and a masked
   match on fetch instructions; a hook on the single function that binds a vertex shader to a declaration and a pixel
   shader captures the original before it is patched.
2. **One microcode can belong to several containers** with different embedded constants, so matching by microcode alone is
   not proof. The renderer prefers a complete identity proof (exact instruction words) over an address or object
   candidate, and rejects a candidate whose words differ from the authoritative loaded program (guard on by default).
   Variants whose only difference is disabled exports are added to the library as exact loaded variants.

Final counters (strict mode): 1,449,793 proven final vertex selections, 0 unproven, in a verified run. A later audit
on the Switch: 2.6 million proven, 0 unproven, 0 missing.

### The shader library

The library is built offline on a PC from the user's disc and is never distributed.

| Step | Result |
|---|---|
| Unreal Engine 3 keeps every compiled shader in its packages (compressed in 128 KB blocks with LZO). A scanner with its own bounds-checked decompressor reads all 2,194 packages in about 30 s | 33,843 chunks, 0 bad blocks, 30,191 distinct containers (228 vertex, 29,963 pixel). All 443 containers the game bound in smoke runs are among them, byte for byte |
| XenosRecomp translates each container to HLSL | 30,135 of 30,191; 56 crash the translator |
| DXC compiles HLSL to SPIR-V, `spirv-val` checks | 30,131 compile; 4 vertex shaders fail (two `BlendWeight0` inputs). So 60 of 30,191 (0.2 %) do not build, none of them used in the smoke runs |
| Packed with a hash of the microcode | one file of about 0.9 GB (versions quoted: 732 MB v24, 929 MB v25) with a separate index file |

Details that mattered:

- The library does not fit a format made for a few hundred shaders. Loading all of it at start took 13.3 s and about 1.7 GB;
  now only the index is loaded (1.0 s) and each SPIR-V is read from the file at its first use.
- Vertex shaders are specialised by constants of the pipeline: which outputs the pixel shader reads (outputs not read are
  not written: scene 66 to 36.6 ms), a normalised input remap, and the constants as dynamic uniform buffers.
- Precision: position is declared invariant and arithmetic "no contraction" so the depth of a multi-pass lit scene
  matches between passes. All 267 vertex shaders are translated this way in the experimental package; the extra cost
  was not measured on the console.
- A whole class of defects (missing terrain, missing weapon effects on one level, a uniformly red screen) came from
  pixel shaders translated at stage 3.2 with an older translator. All 29,852 pixel shaders were retranslated (package v25).
- A discovery mode (`MASSEFFECT_SHADER_DISCOVERY`, with `_MISSING` and `VERTEX_VARIANTS` variants) writes to disk the
  containers and loaded vertex variants the game binds that the library does not have, so coverage can grow with play.

## EDRAM

### What the problem is

On the Xbox 360 the GPU draws into 10 MB of fast memory, EDRAM, and the game "resolves" (copies) the result to normal memory.
A 1280x720 image does not fit, so the game draws it in pieces. The memory is one array of words that the game reads in
different formats at different times. The usual tricks of Unreal Engine 3 on this console:

- Its HDR scene colour is kept as packed **7e3** floats (a 32-bit pixel with three 10-bit floats). In one frame it
  resolves the same words as an ordinary 10-bit format, multiplies them as 8-bit values, and adds light into them as
  7e3 again.
- A depth clear is sometimes drawn through a colour alias, and a colour clear through a depth alias.
- Depth is a 24-bit float (20e4) packed with 8 bits of stencil; the scene is drawn at 2x multisampling and sometimes
  read at 1x.

A Vulkan renderer cannot have one memory with all these views, so it imitates it.

### The modes

`masseffect_native_edram_alias_mode` selects how much of this the renderer imitates.

| Mode | What it does | Status |
|---|---|---|
| 0 | every format has its own image, no sharing | rejected: stale independent views, characters see-through, 7 fps ceiling |
| 2 | physical aliasing of formats | experimental; coloured bands |
| 3 | ownership limited to the scissor of the resolve | experimental; flashes |
| 4 | common physical ownership of EDRAM tiles by depth and colour views, with exact transfers between them | **the default and the only complete one** |

### Mode 4 step by step

1. **Tiles.** EDRAM is divided into tiles, 80x16 pixels at 32 bits and 1x (a 960x544 image is 408 tiles; 1280x720 is 720).
2. **Views.** For each surface the renderer has one Vulkan image per format the game uses on those tiles: 8-bit
   colour, 10-bit colour, 7e3 float, 64-bit colour (raw FP16), and depth with stencil in the encodings D24S8, D24FS8 and
   a half-range form. There are also views that imitate the game's 2x and 4x layouts, but the host never multisamples:
   they keep the sample positions in image coordinates.
3. **Ownership.** For each tile the renderer remembers which view holds the current data.
4. **Sync.** Before a draw, the tiles of its area that another view owns are **imported** (transferred) into the
   draw's view. The area is bounded by a rectangle that is proved from the vertex shader when possible.
5. **Publish.** After the draw, the view becomes the owner of the tiles it wrote.
6. **Transfers** exist in several kinds: depth to depth (a fragment pass; same format copies the float directly), colour
   to colour (compute or, with `masseffect_native_conversion_frag`, a fragment pass), depth to colour (the SDK's rule:
   swap the two 40-word columns of each depth tile), colour to depth, raw 64-bit through `RGBA16_UINT` views so NaN
   and sign bits survive, and stencil.
7. **Stencil** on Maxwell cannot be exported from a shader, so a masked pass per stencil bit (8 passes) was the first
   implementation. It was replaced by a compute shader writing a buffer, copied with `vkCmdCopyBufferToImage` to the
   stencil aspect (3.7 to 5.7 fps).
8. **Redirected clears.** A full-screen depth or stencil clear drawn through a different view would pull all the tiles
   there and back. If the renderer can prove what value the clear writes, it applies it in the owner's view instead, or
   even in a colour owner by decoding the word in that format.
9. **Overwrite proofs.** If a full-screen pass is proved to overwrite everything it touches (the rectangle comes from a
   CPU interpretation of the vertex shader, with constants, indexed two-triangle quads, clipping and OpenGL pixel
   centres; no pixel `KILL`; blend, alpha test and depth state allow it), the import that would load old content is
   skipped.

If a proof fails, the renderer falls back to the conservative transfer; it never guesses. Unsupported conversions fail
explicitly and are counted.

### The cost model

Every transfer runs on a different engine of the GPU than drawing (compute or the copy engine). On this GPU a
switch between engines, and any barrier with a source stage, is a wait for idle. That is why the number of transfers
mattered, and why batching them did not (the per-operation wait was not the cost, the volume of data was).

Measured per frame at 960x544 in Citadel: 4.5 full-screen colour conversions of about 5.4 ms each, because of the two
7e3 round trips described above. They are real data dependencies of the engine, so only the cost of one conversion
can be reduced (fragment passes, G3 in the backlog).

### Depth

- The Xbox 360 depth is a 24-bit float with range [0, 2) in some modes; the host depth is a 32-bit float. The renderer
  maps depth to a half range where needed (the SDK does the same) and clamps with a known lossy limit above 1.
- Quantising the depth the pixel shader writes to 24 bits (`masseffect_native_float24_ps_mode`) reproduces the original
  depth test bit for bit, but it makes the shader write depth, which on this GPU disables early-Z and ZCULL. It must
  stay at 0 (off). It was on by mistake in the first console build and made in-game rendering 0.7-1.0 fps.
- The scene is drawn by the game at 2x multisampling. Two settings imitate it without a multisampled image:
  `masseffect_native_depth_samples_x` keeps both horizontal samples as columns, and
  `masseffect_native_diag_msaa2_phase` (2) cancels the measured half-pixel shift between the 2x prepass and the 1x light
  passes. Without them the lighting of a face showed triangles (a vertical depth sample was lost in a round trip). True
  multisampling stays off: it is not integrated and earlier experience says it can hang this GPU's driver.
- **The depth prepass is skipped** (`masseffect_native_skip_prepass`). Its depth, brought from the 2x view back to 1x,
  disagrees with the material pass on slopes, which dropped whole triangles (black shards, B1). Exact 2x to 1x is still open.

## Resolves

A resolve is the copy of a finished surface from EDRAM to a texture in guest memory. The renderer keeps the result as a
Vulkan image keyed by its guest address, not as bytes, so later draws sample it directly.

| Detail | Why |
|---|---|
| Exponent bias | the engine asks for a signed bias of -3 on its HDR resolves (multiply by 1/8). A dedicated compute shader applies it (`me_resolve_exp_bias.comp`); other nonzero bias combinations fail explicitly |
| Logical size | a resolved texture has a pitch wider than the picture (352 against 322). Sampling the pitch-sized image spread invalid padding into blur. The renderer copies the logical region into a separate image and refreshes it after every partial resolve (`masseffect_native_logical_resolved_size`) |
| Allocation reuse | one address alternated two formats and allocated 1445 images in a run; a small pool of dormant images brought it to 3 (`masseffect_native_reuse_alloc_resolved`) |
| Partial clears | a clear at resolve time uses the clipped rectangle instead of clearing the whole oversized host image |
| Depth resolve | done in guest space with a compute shader, with the SDK's eight sample selectors |

## Draw capture and recording

Because the ring thread is the slowest serial stage, it is built to do as little as possible per draw:

- **Draw records.** Hooks on the game thread record which shader objects each draw uses; the ring pairs them with the draw
  through an allocation-free table and a lock-free queue.
- **State tracking.** The registers written by PM4 are kept as a mirror; viewport, scissor, sampler and fetch caches,
  and pipeline keys are compared without calls; only changed constant ranges are uploaded.
- **Vertex copy.** The vertices a draw reads are copied from guest memory at submit time, because the game rewrites
  its transient UI and movie buffers right after the draw. A copy worker that kept guest pointers once caused
  nondeterministic corruption; the copy is synchronous now (`masseffect_native_uploads_thread` = false). Identical ranges
  of one frame are uploaded once (vertex dedupe, about 70 % of the bytes seen).
- **Deferred recording.** The ring can queue every Vulkan call and let a worker record them
  (`masseffect_native_deferred_recording`), which cut the ring's CPU from about 62 to 40 % (the worker uses about 27 %).
- **Submission.** The first console build had one work slot and synchronous output; three slots and asynchronous
  output removed 207 ms of idle gap per frame.

## Textures, pipelines, output

- Textures are untiled on upload with a cache; mip levels the game provides are uploaded; the texture memory cap is
  per platform. A helper in each pixel shader applies the gamma "signs" of the texture fetch constant; most textures carry them.
- A pipeline is created per distinct state and shader pair; the Vulkan pipeline cache and Mesa's shader cache live on the SD
  card. A background thread re-creates the pipelines of earlier runs at start (`masseffect_native_pipelines_prewarm`).
  A cold start must compile them all on the ring thread, 81 ms each with the per-stage cache.
- The output pass applies the gamma ramp the game uploaded. An early bug: the ramp was enabled before the game had set it
  and the zero-initialised table made every colour black; it is initialised as identity now.

## Resolution

| Layer | What |
|---|---|
| Guest video mode | `video_mode_width` and `video_mode_height` (960x540) make the game size its targets from them; the EDRAM stays exact |
| Internal scene size | `masseffect_scene_width` and `masseffect_scene_height` (960x544): viewport, scene targets, device, back and front buffers. 544 is a multiple of 16 |
| UI | the Scaleform stage is laid out at the internal size (`masseffect_scene_ui`); HUD world-to-screen Y is scaled |
| Post | the tone-map pass still runs at 1280x720: the game upscales inside it |

## Anatomy of a frame

At 1280x720 (t134), from a dump of one frame's GPU marks: scene pass 1 (RGBA16F colour plus D32S8 depth, 2512 draws,
17.3 ms), scene pass 2 (1030 draws, 9.4 ms), per-light imports 3.4 ms, one 59-draw pass 2.6 ms, post chain
2.5 + 1.5 + 1.1 + 0.8 + 0.7 ms, shadow projection 1.5 ms, plus small EDRAM operations. After the prepass was skipped
and 960 adopted, the scene is about 2500 draws, about 42,000 PM4 packets per frame, and the ring thread's per-draw
stopwatch reads: pipeline bind 2.4 µs, draw 2.1, samplers 2.0, indices 1.9, descriptor set 1.5 (13 µs together).

## How correctness was reached

The hard part on the PC was matching the original pixel by pixel, not speed. The method: never accept a still image,
record the system window as video and inspect consecutive frames, compare against the SDK's own renderer on the same
map, capture the contents of resolves and compare numerically, and fix contracts (generic rules), not individual
textures. Defects found and their causes:

| Symptom | Cause | Fix |
|---|---|---|
| white and green polygons, nondeterministic | a vertex copy worker used guest pointers the game had already rewritten | copy at submit time |
| black screen at output | gamma ramp enabled before the game wrote one | identity ramp at creation |
| rejected draws (1,600 per report) | vertex shader variant rewritten by Direct3D did not match the library entry | identify by the ring's microcode and masked fetch match |
| overexposed scene | resolve exponent bias -3 was ignored | biased resolve shader |
| NaN spread into blur | sampled the padded backing image | logical-size sampled image |
| odd-column zero pixels, face and neck triangles | collapsed 4x depth imported into the 2x view repeated samples; the later 2x to 1x round trip lost a vertical phase | horizontal sample grid, measured phase, precise vertex positions, canonical stencil clear |
| clipped utility rectangle (depth -3.7e-9) | Vulkan depth clipping stayed on where the guest disabled clipping | depth clamp follows clip-disable |
| wrong pixel shader selected | address or object candidates were not checked against the loaded words | identity guard on by default |
| missing terrain, weapon effects, red screen | pixel shaders from an older translator | retranslate all pixel shaders (v25) |
| in-game 0.7-1.0 fps on the console | depth export disabled early-Z and ZCULL | float24 mode off |
| black shards on terrain (Switch) | prepass depth disagreeing with material depth | prepass skipped |

All eight smoke maps then ran with 0 rejected draws and 0 draws without shaders, on the PC.

## Settings that define the shipped configuration

The code defaults and the best-performing profile of the development repository differ in a few places; the profile
overrides the defaults. The ones that matter:

| Setting | Value | Note |
|---|---|---|
| `masseffect_native_edram_alias_mode` | 4 | default |
| `masseffect_native_float24_ps_mode` | 0 | must stay 0 |
| `masseffect_native_skip_prepass` | true | default |
| `masseffect_native_depth_samples_x` | false in the profile, true in the code default | see below |
| `masseffect_native_diag_msaa2_phase` | 2 | default (named "diag" but required) |
| `masseffect_native_reuse_alloc_resolved`, `masseffect_native_logical_resolved_size` | true | |
| `masseffect_native_slots_work` | 3 | default |
| `masseffect_native_output_no_wait` | true | default |
| `masseffect_native_conversion_frag` | true in the profile, false in the code | |
| `masseffect_native_deferred_recording` | true in the profile | |
| `masseffect_nvk_cache_per_stage` | true in the profile | needs the patched driver |
| `masseffect_scene_width`, `masseffect_scene_height` | 960, 544 | |

The sources disagree about `masseffect_native_depth_samples_x`: the code default is true (chosen on the PC at the end of
stage 3), the profile used for all later console runs sets it to false with the comment that this gave a correct image
on the PC. Both are recorded; the console runs used false.

Several settings named `diag` (for example the 2x phase) are now required; renaming them is pending.

## Limits

- True 2x multisampling is not integrated.
- Depth above 1 is clamped (a lossy limit), and 24-bit quantisation of depth is off for speed.
- The title screen planet is black in the title-to-menu pan on the Switch only ([known-issues.md](known-issues.md)).
- The water of Eden Prime is absent; it needs the resolved scene depth and colour as prepared textures.
- Some experimental code paths that proved useless for this game remain; they are off.
