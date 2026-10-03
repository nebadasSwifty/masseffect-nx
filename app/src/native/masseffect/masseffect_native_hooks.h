// masseffect - native renderer: access to the shader library the native graphics system has loaded.

#pragma once

namespace masseffect::native {

class ShadersNative;

// The library the native graphics system loaded (nullptr until then), from any thread. Used for pipeline
// prewarming (masseffect_native_draws.cpp).
const ShadersNative* ActiveLibrary();

}  // namespace masseffect::native
