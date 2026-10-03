// Deferred Vulkan command recording (masseffect_native_deferred_recording).
//
// The ring thread translates PM4 and records Vulkan commands; recording in NVK (state flushes, descriptor
// and constant buffer pushes, pushbuffer writes) was ~15 % of that thread. With this on, the ring thread
// only queues each vkCmd* call (arguments copied, arrays into an arena) and a worker thread replays the
// queue into the same command buffers, in the same order. The ring thread waits for the queue to empty
// before anything that needs the recorded state: beginning, ending or resetting a command buffer or pool,
// submitting, updating descriptor sets, destroying or freeing any object. The image is unchanged: the same
// commands reach the driver in the same order.
#pragma once

#include <rex/ui/vulkan/device.h>

namespace masseffect::native::deferred {

using Functions = rex::ui::vulkan::VulkanDevice::Functions;

// The function table the renderer uses: the real one when deferral is off, the proxy when it is on.
const Functions& Table(const Functions& real_fns);

// vkGetDeviceProcAddr through the proxy: vkCmd* functions the renderer fetches by name get wrappers too.
PFN_vkVoidFunction Proc(PFN_vkGetDeviceProcAddr real, VkDevice device, const char* name);

// Ring thread: waits until the worker has recorded everything queued (no-op when off).
void Drain();

// Ring thread: tells the worker about commands the batched mode (masseffect_deferred_native_fast) still holds
// back. Call before the ring thread idles; a no-op otherwise.
void Flush();

}  // namespace masseffect::native::deferred
