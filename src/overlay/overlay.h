// overlay.h — what the rest of cr-srt sees of the overlay: one call a frame from the Present wrapper, one at unload.
#pragma once

namespace crsrt::overlay {

// Render thread, every wrapper call, before the game presents: a swapchain seen for the first time gets its vtable
// swapped (swap.cpp), and one draw is armed for the Present that follows. Also where the vtable goes back at unload.
void on_wrapper(void *swapchain, void *device, void *queue);

// Unloader thread, before the hooks come off: true = vtable restored, the GPU done with our lists, ImGui torn down,
// safe to unhook and unmap; false = the DLL must stay mapped, and the log says why.
bool unload();

}  // namespace crsrt::overlay
