// renderer.cpp
#define WIN32_LEAN_AND_MEAN
#define WIDL_EXPLICIT_AGGREGATE_RETURNS  // MinGW's d3d12.h: struct returns (the heap-start handles) by the MSVC ABI
#include "overlay/renderer.h"

#include "log.h"
#include "overlay/ui.h"

#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "imgui.h"
#include "imgui_impl_dx12.h"

// IM_ASSERT (imgui_user_config.h) logs instead of calling abort() inside the game; the first 20 are enough.
void crsrt_imgui_assert(const char *expr, const char *file, int line) {
    static volatile LONG n;
    const char *base = file;
    for (const char *p = file; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    if (InterlockedIncrement(&n) <= 20) crsrt::log::write("imgui assert: %.160s (%.60s:%d)", expr, base, line);
}

namespace crsrt::renderer {

namespace {

constexpr int kFrames = 3;     // command allocators in flight, and the backend's NumFramesInFlight
constexpr int kSrvSlots = 16;  // shader-visible descriptors for ImGui's textures: the font atlas, and the atlas regrown

ID3D12Device *g_target_dev;  // what the wrapper last handed over
ID3D12CommandQueue *g_target_q;

// The render state, built at the first frame and rebuilt when the format, the device or the queue changes.
ID3D12Device *g_dev;
ID3D12CommandQueue *g_q;
ID3D12DescriptorHeap *g_rtv_heap, *g_srv_heap;
UINT g_rtv_step, g_srv_step;
D3D12_CPU_DESCRIPTOR_HANDLE g_srv_cpu0;
D3D12_GPU_DESCRIPTOR_HANDLE g_srv_gpu0;
bool g_srv_used[kSrvSlots];
ID3D12CommandAllocator *g_alloc[kFrames];
ID3D12GraphicsCommandList *g_list;
ID3D12Fence *g_fence;
HANDLE g_fence_event;
UINT64 g_fence_at[kFrames], g_fence_last;
UINT g_frame;  // frames drawn since the last build; picks the allocator
long g_drawn, g_skipped;
DXGI_FORMAT g_format;
bool g_ready;
bool g_broken;  // a build or a frame failed: nothing is drawn again this session
float g_font_px;
LARGE_INTEGER g_qpf, g_prev;

// The overlay's own cost per 600 frames, in microseconds: the wait for our allocator's fence (normally zero), and
// the frame itself, from the buffer lookup through ImGui's build and draw to the list's execution.
struct Cost {
    int64_t sum = 0, max = 0;
    void add(int64_t us) {
        sum += us;
        max = std::max(us, max);
    }
} g_wait_cost, g_frame_cost;
long g_cost_frames;
constexpr long kCostEvery = 600;

int64_t now_us() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000000 / g_qpf.QuadPart;
}

bool ok(HRESULT hr, const char *what) {
    if (SUCCEEDED(hr)) return true;
    log::write("overlay: %s failed, hr %08lx", what, static_cast<unsigned long>(hr));
    return false;
}

template <class T>
void drop(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void srv_alloc(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu) {
    int i = 0;
    while (i < kSrvSlots && g_srv_used[i]) i++;
    if (i == kSrvSlots) {
        log::write("overlay: SRV heap full, slot 0 reused");
        i = 0;
    }
    g_srv_used[i] = true;
    cpu->ptr = g_srv_cpu0.ptr + static_cast<SIZE_T>(i) * g_srv_step;
    gpu->ptr = g_srv_gpu0.ptr + static_cast<UINT64>(i) * g_srv_step;
}

void srv_free(ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    SIZE_T i = (cpu.ptr - g_srv_cpu0.ptr) / g_srv_step;
    if (i < kSrvSlots) g_srv_used[i] = false;
}

void release_all() {
    drop(g_list);
    for (auto &a : g_alloc) drop(a);
    drop(g_fence);
    drop(g_srv_heap);
    drop(g_rtv_heap);
    if (g_fence_event) {
        CloseHandle(g_fence_event);
        g_fence_event = nullptr;
    }
    for (bool &used : g_srv_used) used = false;
    for (UINT64 &at : g_fence_at) at = 0;
    g_fence_last = 0;
    g_frame = 0;
    g_ready = false;
}

// Our last list done on the GPU, waiting at most `ms`; false = still running, nothing may be released.
bool gpu_idle(DWORD ms) {
    if (!g_fence || g_fence->GetCompletedValue() >= g_fence_last) return true;
    g_fence->SetEventOnCompletion(g_fence_last, g_fence_event);
    WaitForSingleObject(g_fence_event, ms);
    return g_fence->GetCompletedValue() >= g_fence_last;
}

bool build(const DXGI_SWAP_CHAIN_DESC &d) {
    g_dev = g_target_dev;
    g_q = g_target_q;
    g_format = d.BufferDesc.Format;
    D3D12_DESCRIPTOR_HEAP_DESC rh{}, sh{};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = kFrames;
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = kSrvSlots;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    bool good = ok(g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_rtv_heap)), "RTV heap") &&
                ok(g_dev->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&g_srv_heap)), "SRV heap") &&
                ok(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "fence");
    for (int i = 0; good && i < kFrames; i++)
        good = ok(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i])),
                  "command allocator");
    good = good &&
           ok(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list)),
              "command list") &&
           ok(g_list->Close(), "first Close");
    if (good) {
        g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_fence_event) {
            log::write("overlay: CreateEvent failed");
            good = false;
        }
    }
    if (!good) {
        release_all();
        return false;
    }
    g_rtv_step = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_srv_step = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_srv_cpu0 = g_srv_heap->GetCPUDescriptorHandleForHeapStart();
    g_srv_gpu0 = g_srv_heap->GetGPUDescriptorHandleForHeapStart();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;  // nothing to remember yet: no windows
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefaultVector();  // scalable, so one size read off the height is enough
    g_font_px = ui::font_px(static_cast<float>(d.BufferDesc.Height));
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = g_dev;
    ii.CommandQueue = g_q;
    ii.NumFramesInFlight = kFrames;
    ii.RTVFormat = g_format;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = g_srv_heap;
    ii.SrvDescriptorAllocFn = srv_alloc;
    ii.SrvDescriptorFreeFn = srv_free;
    // the device objects now, not lazily in NewFrame: IM_ASSERT returns here, so a failure must be caught, not asserted
    if (!ImGui_ImplDX12_Init(&ii) || !ImGui_ImplDX12_CreateDeviceObjects()) {
        log::write("overlay: ImGui DX12 init or its device objects failed");
        if (io.BackendRendererUserData) ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext();
        release_all();
        return false;
    }
    QueryPerformanceFrequency(&g_qpf);
    g_prev.QuadPart = 0;
    g_ready = true;
    log::write("overlay: built on device %p queue %p, format %d, %d frames in flight, font %d px", g_dev, g_q,
               static_cast<int>(g_format), kFrames, static_cast<int>(g_font_px));
    return true;
}

}  // namespace

void set_target(ID3D12Device *device, ID3D12CommandQueue *queue) {
    g_target_dev = device;
    g_target_q = queue;
}

bool teardown(DWORD ms) {
    if (!gpu_idle(ms)) return false;
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext();
    }
    release_all();
    return true;
}

void frame(IDXGISwapChain *swapchain) {
    if (g_broken) return;
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(swapchain->GetDesc(&d)) || !d.BufferDesc.Width || !d.BufferDesc.Height) return;
    if (g_ready && (d.BufferDesc.Format != g_format || g_target_dev != g_dev || g_target_q != g_q)) {
        log::write("overlay: format %d -> %d, device %p -> %p, queue %p -> %p: rebuilding", static_cast<int>(g_format),
                   static_cast<int>(d.BufferDesc.Format), g_dev, g_target_dev, g_q, g_target_q);
        if (!teardown(1000)) {
            log::write("overlay: GPU still on our list after 1 s - drawing stops");
            g_broken = true;
            return;
        }
    }
    if (!g_ready && !build(d)) {
        g_broken = true;
        return;
    }

    UINT f = g_frame % kFrames;
    const int64_t t_wait = now_us();
    if (g_fence->GetCompletedValue() < g_fence_at[f]) {  // never an unbounded wait on the render thread
        g_fence->SetEventOnCompletion(g_fence_at[f], g_fence_event);
        WaitForSingleObject(g_fence_event, 100);
        if (g_fence->GetCompletedValue() < g_fence_at[f]) {
            if (g_skipped++ % 600 == 0)
                log::write("overlay: allocator %u still busy after 100 ms, frame skipped (%ld so far)", f, g_skipped);
            return;
        }
    }
    const int64_t t_frame = now_us();
    g_wait_cost.add(t_frame - t_wait);
    IDXGISwapChain3 *sc3 = nullptr;
    if (FAILED(swapchain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&sc3))) || !sc3) {
        log::write("overlay: swapchain %p has no IDXGISwapChain3 - drawing stops", swapchain);
        g_broken = true;
        return;
    }
    UINT bb = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    ID3D12Resource *buf = nullptr;
    HRESULT hr = swapchain->GetBuffer(bb, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&buf));
    if (FAILED(hr) || !buf) {
        if (g_skipped++ % 600 == 0)
            log::write("overlay: GetBuffer(%u) hr %08lx, frame skipped", bb, static_cast<unsigned long>(hr));
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(f) * g_rtv_step;
    g_dev->CreateRenderTargetView(buf, nullptr, rtv);
    if (!ok(g_alloc[f]->Reset(), "allocator Reset") || !ok(g_list->Reset(g_alloc[f], nullptr), "list Reset")) {
        buf->Release();
        g_broken = true;
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = buf;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_list->SetDescriptorHeaps(1, &g_srv_heap);

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(d.BufferDesc.Width), static_cast<float>(d.BufferDesc.Height));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = g_prev.QuadPart ? static_cast<float>(static_cast<double>(now.QuadPart - g_prev.QuadPart) /
                                                    static_cast<double>(g_qpf.QuadPart))
                               : 0.0f;
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
    g_prev = now;
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    ui::draw(io.DisplaySize.x, io.DisplaySize.y, g_font_px, g_drawn);
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(),
                                  g_list);  // the first one also uploads the font atlas, and waits

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &b);
    if (!ok(g_list->Close(), "list Close")) {
        buf->Release();
        g_broken = true;
        return;
    }
    ID3D12CommandList *lists[] = {g_list};
    g_q->ExecuteCommandLists(1, lists);
    hr = g_q->Signal(g_fence, g_fence_last + 1);
    if (SUCCEEDED(hr)) {
        g_fence_at[f] = ++g_fence_last;
    } else {
        log::write("overlay: Signal hr %08lx - drawing stops", static_cast<unsigned long>(hr));
        g_broken = true;
    }
    buf->Release();
    g_frame++;
    g_drawn++;
    g_frame_cost.add(now_us() - t_frame);
    if (++g_cost_frames == kCostEvery) {
        log::write("overlay: own cost over %ld frames: frame avg %ld us, max %ld us; fence wait avg %ld us, max %ld us",
                   kCostEvery, static_cast<long>(g_frame_cost.sum / kCostEvery), static_cast<long>(g_frame_cost.max),
                   static_cast<long>(g_wait_cost.sum / kCostEvery), static_cast<long>(g_wait_cost.max));
        g_frame_cost = g_wait_cost = {};
        g_cost_frames = 0;
    }
    if (g_drawn == 1 || g_drawn % 3600 == 0)
        log::write("overlay: frame %ld drawn, %ux%u format %d, back buffer %u", g_drawn, d.BufferDesc.Width,
                   d.BufferDesc.Height, static_cast<int>(d.BufferDesc.Format), bb);
}

bool ready() { return g_ready; }
long frames_drawn() { return g_drawn; }
long frames_skipped() { return g_skipped; }

}  // namespace crsrt::renderer
