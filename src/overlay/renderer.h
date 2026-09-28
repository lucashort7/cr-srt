// renderer.h — Dear ImGui on the game's own D3D12 device and Direct queue, one command list a frame drawn onto the
// back buffer right before it is presented. No platform backend: nothing reads input or touches the cursor, so the
// display size and the frame time are set by hand. Render thread only, except teardown at unload.
#pragma once

#include <windows.h>

struct IDXGISwapChain;
struct ID3D12Device;
struct ID3D12CommandQueue;

namespace crsrt::renderer {

// The device and queue the next frame must use; a change rebuilds everything on the next frame.
void set_target(ID3D12Device *device, ID3D12CommandQueue *queue);

// Draws ui::draw's frame on the swapchain's current back buffer: arrives in PRESENT, leaves in PRESENT.
void frame(IDXGISwapChain *swapchain);

// Waits at most `ms` for the GPU to finish our last list, then releases ImGui and every object. False = the GPU is
// still on it and nothing was released.
bool teardown(DWORD ms);

bool ready();
long frames_drawn();
long frames_skipped();

}  // namespace crsrt::renderer
