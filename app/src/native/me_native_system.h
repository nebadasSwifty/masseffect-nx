// Native graphics system (see me_native_system.cpp).
#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace me::native {

// true when MASSEFFECT_NATIVE=1: the app installs the native system instead of Xenos emulation.
bool Enabled();

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem();

// Called on the game thread when Direct3D binds a shader object (sub_8222F850), before it patches the
// vertex shader: records which library entry the microcode at that physical address belongs to, so the
// ring can identify IM_LOADs by address No-op without the native system.
void NoteShaderObject(const uint8_t* base, uint32_t object, bool vertex);

// True when the shader object was matched to a library entry (diagnostics).
bool HasEntry(uint32_t object);

// Called on the game thread by the four Direct3D draw functions (src/me_d3d_trace.cpp): the bound VS
// and PS objects and the call's primitive type and counts. The ring pairs each DRAW_INDX with these
// records in order, because the ring's own shader loads can be immediate
// copies Direct3D patched. No-op without the native system.
void NoteDrawCall(const uint8_t* base, uint32_t type, uint32_t r5, uint32_t r6, uint32_t r7);

// Called by the ring thread after it writes the read pointer back: wakes the game's D3D GPU wait
// (src/native/me_ring_wait.cpp, sub_8222FA98).
void NotifyRingProgress();

}  // namespace me::native
