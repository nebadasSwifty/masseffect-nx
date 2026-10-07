# Mass Effect (Xbox 360) for Nintendo Switch: static recompilation host application.
#
# The game code is not in this repository: run tools/codegen.sh first (it produces generated/, which holds
# the code recompiled from your own default.xex and the CMake glue). A cross build cannot run rexglue itself.
# Build with tools/build_nro.sh (devkitA64 Docker image), or by hand:
#   cmake -S app -B out/nx -G Ninja -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=tools/switch/cmake/switch-devkitA64.cmake \
#     -DREXGLUE_SWITCH_NVK_SDK=<mesa sdk>/opt/devkitpro/portlibs/switch \
#     -DMASSEFFECT_PREGENERATED_SHADER_DIR=<dir with the SPIR-V .inc files>
# Output: out/nx/masseffect.nro ("Mass Effect" in the Homebrew Menu).

cmake_minimum_required(VERSION 3.25)
project(masseffect LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK is built from source in this repository unless another tree is given.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

include(generated/rexglue.cmake)

set(MASSEFFECT_SOURCES
    src/main.cpp
    # Hooks on the game's Direct3D entry points (feed the native renderer, internal resolution, optional timing).
    src/me_d3d_trace.cpp
    src/me_shader_dump.cpp
    src/native/me_native_system.cpp
    src/native/me_ring_wait.cpp
    src/native/me_resolution.cpp
    src/native/me_physx.cpp
    src/native/me_audio_hooks.cpp
    src/native/me_hot_guest.cpp
    src/native/me_hang_watchdog.cpp
    src/native/me_render_ring.cpp
    src/native/masseffect/masseffect_native_draws.cpp
    src/native/masseffect/masseffect_deferred_recording.cpp
    src/native/masseffect/masseffect_native_targets.cpp
    src/native/masseffect/masseffect_native_shaders.cpp
    src/native/masseffect/masseffect_native_textures_pool.cpp
    src/native/masseffect/masseffect_shader_library.cpp
    src/native/masseffect/me_masseffect_glue.cpp
    src/me_pgo.cpp
)
add_executable(masseffect ${MASSEFFECT_SOURCES})
# Line tables only (no code change) for the app's own sources, so console stack profiles can be resolved to source
# lines with addr2line; the NRO itself carries no debug sections.
set_source_files_properties(${MASSEFFECT_SOURCES} PROPERTIES COMPILE_OPTIONS "-g1")

# SPIR-V initializers of the native renderer's compute/utility shaders (src/native/masseffect/shaders). The
# devkitA64 image has no glslang: tools/build_nro.sh generates them on the host (SPIR-V is platform independent)
# and passes MASSEFFECT_PREGENERATED_SHADER_DIR.
set(MASSEFFECT_PREGENERATED_SHADER_DIR "" CACHE PATH "Host-generated SPIR-V initializers (*.inc)")
if(MASSEFFECT_PREGENERATED_SHADER_DIR)
    set(MASSEFFECT_GENERATED_SHADER_DIR "${MASSEFFECT_PREGENERATED_SHADER_DIR}")
else()
    find_program(MASSEFFECT_GLSLANG_VALIDATOR glslangValidator NO_CMAKE_FIND_ROOT_PATH REQUIRED)
    set(MASSEFFECT_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
endif()
set(MASSEFFECT_EDRAM_SHADER_INCLUDES)
foreach(_shader masseffect_edram_7e3_to_rgba8 masseffect_edram_rgba8_to_7e3 masseffect_edram_16f_to_16f me_resolve_exp_bias me_edram_depth_to_rgba8 me_edram_depth_to_16f me_edram_import me_edram_color_to_depth me_edram_depth_to_depth me_edram_depth_to_stencil me_edram_color_to_stencil me_edram_depth_to_depth_msaa2 me_edram_depth_msaa2_to_depth_1x me_edram_depth_to_stencil_msaa2 me_edram_depth_msaa2_to_stencil_1x me_edram_raw64_to_rgba8 me_edram_rgba8_to_raw64 me_edram_raw64_to_16f me_edram_16f_to_raw64 me_edram_raw64_to_raw64 me_edram_depth_to_raw64 me_edram_raw64_to_depth me_depth_resolve_guestspace me_depth_resolve_guestspace_msaa2 me_edram_stencil_to_buffer me_edram_stencil_msaa2_to_buffer me_edram_color_stencil_to_buffer me_edram_color_to_color me_edram_r64_to_r64 me_edram_r16g16_to_rgba8 me_edram_rgba8_to_r16g16 me_edram_r16g16_to_16f me_edram_16f_to_r16g16 me_resolve_7e3_to_unorm10 me_resolve_exp_bias_frag)
    set(_extension comp)
    if(_shader STREQUAL "me_edram_import")
        set(_extension vert)
    elseif(_shader STREQUAL "me_edram_color_to_depth" OR _shader STREQUAL "me_edram_depth_to_depth" OR _shader STREQUAL "me_edram_depth_to_stencil" OR _shader STREQUAL "me_edram_color_to_stencil" OR _shader STREQUAL "me_edram_depth_to_depth_msaa2" OR _shader STREQUAL "me_edram_depth_msaa2_to_depth_1x" OR _shader STREQUAL "me_edram_depth_to_stencil_msaa2" OR _shader STREQUAL "me_edram_depth_msaa2_to_stencil_1x" OR _shader STREQUAL "me_edram_raw64_to_depth" OR _shader STREQUAL "me_edram_color_to_color" OR _shader STREQUAL "me_edram_r64_to_r64" OR _shader STREQUAL "me_resolve_exp_bias_frag")
        set(_extension frag)
    endif()
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/src/native/masseffect/shaders/${_shader}.${_extension}")
    set(_output "${MASSEFFECT_GENERATED_SHADER_DIR}/${_shader}.inc")
    set(_target_flags)
    if(_shader STREQUAL "me_edram_stencil_to_buffer" OR _shader STREQUAL "me_edram_color_stencil_to_buffer" OR _shader STREQUAL "me_edram_stencil_msaa2_to_buffer" OR _shader STREQUAL "me_edram_depth_to_depth_msaa2" OR _shader STREQUAL "me_edram_depth_msaa2_to_depth_1x" OR _shader STREQUAL "me_edram_depth_to_stencil_msaa2" OR _shader STREQUAL "me_edram_depth_msaa2_to_stencil_1x" OR _shader STREQUAL "me_depth_resolve_guestspace_msaa2")
        set(_target_flags --target-env vulkan1.2)
        set(_output "${MASSEFFECT_GENERATED_SHADER_DIR}/${_shader}_generated.inc")
    endif()
    if(MASSEFFECT_PREGENERATED_SHADER_DIR)
        if(NOT EXISTS "${_output}")
            message(FATAL_ERROR "Missing pregenerated shader ${_output}")
        endif()
        list(APPEND MASSEFFECT_EDRAM_SHADER_INCLUDES "${_output}")
        continue()
    endif()
    add_custom_command(OUTPUT "${_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${MASSEFFECT_GENERATED_SHADER_DIR}"
        COMMAND "${MASSEFFECT_GLSLANG_VALIDATOR}" -V ${_target_flags} -x -o "${_output}" "${_source}"
        DEPENDS "${_source}" VERBATIM)
    list(APPEND MASSEFFECT_EDRAM_SHADER_INCLUDES "${_output}")
endforeach()
add_custom_target(masseffect_edram_shaders DEPENDS ${MASSEFFECT_EDRAM_SHADER_INCLUDES})
add_dependencies(masseffect masseffect_edram_shaders)
target_include_directories(masseffect PRIVATE
    "${MASSEFFECT_GENERATED_SHADER_DIR}"
    src
    src/native/masseffect
    src/native/masseffect/shaders
    "${REXSDK_DIR}/src/core"
    "${REXSDK_DIR}/src/ui/shaders")

# Native shader/package fingerprints use the SDK's header-only xxHash API.
find_path(MASSEFFECT_XXHASH_INCLUDE xxhash.h
    HINTS "${REXSDK_DIR}/thirdparty/xxHash"
    NO_CMAKE_FIND_ROOT_PATH REQUIRED)
target_include_directories(masseffect PRIVATE "${MASSEFFECT_XXHASH_INCLUDE}")

# Link the SDK; ReXGlue's Xenos GPU plugin stays available as a fallback (cvar masseffect_renderer_native=false).
rexglue_setup_target(masseffect GPU_PLUGINS xenos)

# CPU-only SDK sources reused by the native renderer (draw-extent estimation, shader microcode interpretation).
# The GPU plugin's private symbols cannot satisfy application-side use.
target_sources(masseffect PRIVATE
    "${REXSDK_DIR}/src/graphics/register_file.cpp"
    "${REXSDK_DIR}/src/graphics/format/ucode.cpp"
    "${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp")

# The SDK compiles itself with these (directory scope), but they do not reach the generated game code, which is
# built in this directory. GCC's default for GNU C++ is -ffp-contract=fast: it fuses separate PPC fmul/fadd into
# aarch64 FMA and results drift from the Xbox 360.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    foreach(target masseffect masseffect_recomp)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -ffp-contract=off -fno-strict-aliasing)
        endif()
    endforeach()
endif()

# No errno side effect on sqrt/etc. and no FP-trap assumptions in the recompiled game code: removes the libm
# slow-path call after every fsqrt (-1.1 % instructions on the hot functions). Results are identical (no guest
# code reads the host FP exception flags).
option(MASSEFFECT_FAST_MATH "-fno-math-errno -fno-trapping-math on the recompiled game code" ON)
if(MASSEFFECT_FAST_MATH AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND TARGET masseffect_recomp)
    target_compile_options(masseffect_recomp PRIVATE -fno-math-errno -fno-trapping-math)
endif()

# Optimisation level of the recompiled game code only (Cortex-A57: 48 KB I-cache, ~50 MB of generated .text; a
# smaller -Os/-O2 build may miss less than the default -O3). Empty = keep the default.
set(MASSEFFECT_GEN_OPT "" CACHE STRING "Extra optimisation flag for masseffect_recomp, e.g. -Os or -O2")
if(MASSEFFECT_GEN_OPT AND TARGET masseffect_recomp)
    target_compile_options(masseffect_recomp PRIVATE ${MASSEFFECT_GEN_OPT})
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # LTO on the game code and ours (not the SDK libraries). With tools/direct_calls.py (calls to unhooked
    # functions go to the non-weak __imp__) small game functions inline across files. Measured: no gain, much
    # longer builds, ~16 GB for the build container, so it is off by default.
    option(MASSEFFECT_LTO "LTO on the game and app code (Switch)" OFF)
    if(MASSEFFECT_LTO)
        foreach(target masseffect_recomp masseffect)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE -flto -fno-fat-lto-objects)
            endif()
        endforeach()
        target_link_options(masseffect PRIVATE -flto=2 -flto-partition=balanced)
    endif()
    # Profile-guided optimization in two builds: MASSEFFECT_PGO=generate (instrumented; the game dumps .gcda files
    # to sdmc:/switch/masseffect-nx/pgo every 60 s, src/me_pgo.cpp), then copy them to ../pgo and rebuild the SAME
    # build dir with MASSEFFECT_PGO=use. Do not regenerate code or edit sources in between.
    # -fno-profile-values: libgcov's indirect-call profiling reads TLS via tpidr_el0, which is 0 on Horizon.
    get_filename_component(MASSEFFECT_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" ABSOLUTE)
    set(MASSEFFECT_PGO "" CACHE STRING "PGO: empty, generate or use")
    if(MASSEFFECT_PGO)
        if(MASSEFFECT_PGO STREQUAL "generate")
            set(MASSEFFECT_PGO_OPTIONS -fprofile-generate=${MASSEFFECT_PGO_DIR}
                "-fprofile-prefix-path=${CMAKE_BINARY_DIR}" -fprofile-update=single -fno-profile-values)
            target_link_options(masseffect PRIVATE -fprofile-generate -Wl,--wrap=fopen)
            target_compile_definitions(masseffect PRIVATE MASSEFFECT_PGO_GENERATE=1
                                       "MASSEFFECT_PGO_DIR=\"${MASSEFFECT_PGO_DIR}\"")
        elseif(MASSEFFECT_PGO STREQUAL "use")
            set(MASSEFFECT_PGO_OPTIONS -fprofile-use=${MASSEFFECT_PGO_DIR} "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
                -fprofile-partial-training -fno-profile-values -fprofile-correction -Wno-missing-profile
                -Wno-error=coverage-mismatch)
            target_link_options(masseffect PRIVATE -fprofile-use -fprofile-partial-training -fprofile-correction)
        else()
            message(FATAL_ERROR "MASSEFFECT_PGO must be empty, generate or use (is '${MASSEFFECT_PGO}')")
        endif()
        foreach(target masseffect_recomp masseffect)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${MASSEFFECT_PGO_OPTIONS})
            endif()
        endforeach()
    endif()
    # Hot functions grouped at the start of .text: the A57 has 48 KB of instruction cache and a small TLB, and the
    # game code is tens of MB. The list (function_order.ld) comes from console stack sampling.
    option(MASSEFFECT_FUNCTION_ORDER "Group hot functions at the start of .text" ON)
    if(MASSEFFECT_FUNCTION_ORDER)
        target_link_options(masseffect PRIVATE
            "-Wl,--section-ordering-file,${CMAKE_CURRENT_SOURCE_DIR}/function_order.ld")
        set_property(TARGET masseffect APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/function_order.ld")
    endif()
    # On the SD card: sdmc:/switch/masseffect-nx/masseffect-nx.nro with masseffect.toml and game_root/ beside it.
    set(MASSEFFECT_VERSION "1.0.0" CACHE STRING "NRO display version")
    rexglue_switch_add_nro(masseffect
        NAME "Mass Effect"
        AUTHOR "NebadasSwifty"
        VERSION "${MASSEFFECT_VERSION}"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/../extras/icon.jpg")
endif()
