# How the game's shaders are translated

## In short

The Xbox 360's GPU programs ([shaders](glossary.md#shader-vertex-shader-pixel-shader-fragment-shader)) are Xenos [microcode](glossary.md#microcode-container). The Switch's GPU
cannot run them, and translating them while playing causes stutter and costs CPU, so the port translates **all of them
before the game runs** and keeps the result in one local file, the shader library (`masseffect_shaders.mesp`, plus its
index `masseffect_shaders.mesp.idx`). The renderer only looks shaders up in it, by a fingerprint of the original microcode.
Nothing from the game is in this repository: the shaders are read from the user's own disc. This page is the overview;
the exact file formats, command lines and every option are in [../shaders/README.md](../shaders/README.md), which is the
authority.

## From the disc to the GPU

```
game files (Unreal packages *.xxx, LZO-compressed chunks)
   |  ue3_shader_scan                         finds the containers (also: a run-time dump, app/src/me_shader_dump.cpp)
   v
containers    vs_<hash>.bin / ps_<hash>.bin   original Xbox 360 shader containers, big-endian
   |  masseffect_hlsl  (XenosRecomp + this port's changes)
   v
HLSL          vs_<hash>.hlsl / ps_<hash>.hlsl one self-contained file each
   |  DXC (-spirv, Vulkan 1.2) + spirv-val
   v
SPIR-V        vs_<hash>.spv / ps_<hash>.spv
   |  me_pack_shaders
   v
masseffect_shaders.mesp  +  masseffect_shaders.mesp.idx
```

1. **Find the containers.** Mass Effect is built on Unreal Engine 3, and its packages (`*.xxx`) hold a shader cache in
   compressed chunks (LZO1X, in 128 KB blocks). `ue3_shader_scan` finds the chunks by their header, decompresses them,
   and looks for shader containers: the format the Xbox 360 shader compiler writes (a signature `0x102A11xx` whose low bit
   says vertex or pixel shader, a virtual size and a physical size). On a complete disc it finds 403,857 containers, of
   which **30,191 are distinct**.
2. **Container to HLSL.** [XenosRecomp](glossary.md#xenosrecomp) (by hedge-dev) turns each container into one HLSL file
   that starts with a copy of `shader_common.h`, so it compiles on its own.
3. **HLSL to SPIR-V.** DXC compiles it for Vulkan 1.2 (`ps_6_6` or `vs_6_6`), and `spirv-val` checks it. A shader that
   fails either is not packed.
4. **The package.** Each entry holds the original container and its SPIR-V, sorted by a 64-bit XXH3 hash of the
   container. At run time the renderer hashes the container the game hands to Direct3D, binary-searches the table and
   compares the container bytes, so a hash collision can never select another shader.
5. **The index.** The full package is about 920 MB. The index (about 48 MB) holds the table and the containers only, so the
   game reads the SPIR-V of a shader from the package the first time it is used. This cut the library load from 13.3 to
   1.0 s and freed about 800 MB of the process's tight memory budget. A package without a matching index still loads, in
   full; `masseffect_shaders_index = false` turns the index off.

The installer page runs the same steps in the browser with WebAssembly builds of the same programs (`shaders/wasm/`); its
DXC build is not finished, see [building.md](building.md) for the command line way.

## Containers that do not make it

On the complete disc, 56 of the 30,191 containers crash the translator (probably malformed false positives of the scan)
and 4 vertex shaders translate but DXC rejects them. They are left out, so the package has **30,131** shaders. An
installer or script must treat a failed container as "skip it", never as a fatal error; `build_shader_spirv.sh` refuses
to pack when anything failed unless `ALLOW_FAILURES=1`, so that a regression is noticed.

## Fixes needed for a correct image

XenosRecomp is the upstream translator with changes for this game, all marked `MASSEFFECT` in the source:

- **Sampler arrays.** Unreal's `LightMapTextures` (`s5..s7`) is one sampler array covering several registers. Upstream only
  defined the first; every register now gets its definitions.
- **2008 containers** whose physical part is not a multiple of 12 bytes (844 and 856 byte ones exist) are accepted.
- **Relocated literal definitions.** Float definitions whose physical range overlaps the microcode or lies outside the
  physical block are ignored and the real guest registers are read.
- **Direct3D's own shaders** (those with no constant table, or an empty one) are linked by hardware register instead of by
  semantic names.
- **Xbox 360 semantics.** Simultaneous vector and scalar ALU results are computed with the operands as they were before
  the instruction; the alpha test has all eight comparison functions; texture signs (unsigned-biased and gamma) are applied
  in the sampling helpers; the position is mapped to the host's clip space; `setp_inv` follows Xenia. An opt-in `precise`
  position contract exists (`MASSEFFECT_SHADER_PRECISE_POSITION=1`).

## Changes that made the shaders faster

| Change | Effect |
|---|---|
| Constants come through a dynamic uniform buffer instead of a 64-bit pointer | one buffer, set by differences between draws (see [mesa.md](mesa.md)) |
| 1/size of a texture comes from the shared constants instead of a size query | no size query per sample |
| Vertex shaders without the strict "no contraction" flag except on the position path; input remap with `select()` | scene 78 to 66 ms |
| A vertex shader writes only the outputs the pixel shader of that pipeline reads (NVK links no varyings) | **scene 66 to 36.6 ms**; the frame became CPU bound |
| Vertex input remap codes normalised so identity slots take the shader's fast path | scene 41.9 to 36.9 ms |
| Branch-free texture-sign helper, no-signs fast path, fused multiply-add in vertex shaders, early-Z for alpha-tested writers | measured: no gain |

The evidence for each, with run names, is in [optimization-paths.md](optimization-paths.md), section 5.4. The scene is
bound by vertex work and per-draw fixed cost much more than by pixel shader ALU.

## How translator changes were checked

- `shaders/tests/` has the code generation tests of the translator that need no game files
  (`test_shader_precise_codegen.sh`, `test_shader_alu_parallel_codegen.sh`), an index test (index load equals full load,
  `test_shader_index.cpp`) and a vertex declaration test.
- The package is deterministic for a given set of (container, SPIR-V) pairs, and `shaders/tools/package_reference.py` is an
  independent Python reader and writer written from the format description that reproduces the C++ output byte for byte.
- A change that should not alter the output was compared on hundreds of containers (the HLSL had to be identical).
- `shaders/tools/validate_shader_package.sh` runs every SPIR-V of a package through SPIRV-Tools.
- Image correctness is judged on the console from many captures, never from two or three
  ([measuring.md](measuring.md)).

## What you would change for another game

The package format, the lookup by container hash, the index, the packer and the tools are game independent. What is
specific: where the containers are stored (here, Unreal packages with LZO chunks), and the translator fixes above. See
[porting-another-game.md](porting-another-game.md).
