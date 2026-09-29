// swap.cpp — how the overlay gets its turn at Present. The first time the wrapper hands over a swapchain, that object
// gets a copy of its class's vtable with slot 8 (Present) pointing here: the game's Present then lands in hk_present
// after the game's own barrier to PRESENT and its submit, where drawing is safe. Only this one object is touched,
// never the shared vtable or the Present code, so nothing is shared with RTSS or the Steam overlay. The swapchain,
// device and queue are the ones the game uses (Streamline's proxies, or FSR frame generation's swapchain).
#include "overlay/overlay.h"

#include "log.h"
#include "memory.h"
#include "overlay/renderer.h"

#include <d3d12.h>
#include <dxgi.h>
#include <windows.h>

namespace crsrt::overlay {

namespace {

constexpr int kMaxClasses = 8;  // distinct vtables given a copy; every object of one class shares its copy
constexpr int kMaxSlots = 512;
// IUnknown 3, IDXGIObject 4, DeviceSubObject 1, SwapChain 10, 1: 11, 2: 7, 3: 4, 4: 1
constexpr int kSwapChain4Slots = 41;

using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);

// One swapped class: its original vtable, our copy, and the allocation behind the copy (block[0] is the RTTI slot).
struct Swapped {
    void *object;
    void **original;
    void **table;
    void **block;
    int slots;
};
Swapped g_swapped[kMaxClasses];
int g_count;
void **g_refused;  // a vtable too short to be a swapchain's: logged once, never scanned again

volatile LONG g_stop_draw, g_restore_requested, g_restore_done, g_present_inflight;
volatile LONG g_armed;  // one draw per wrapper call: the wrapper's retry after 0x887A0001 presents the same frame again
long g_presents;

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain *sc, UINT sync, UINT flags) {
    InterlockedIncrement(&g_present_inflight);
    void **vt = *reinterpret_cast<void ***>(sc);
    PresentFn original = nullptr;
    for (int i = 0; i < g_count && !original; i++)
        if (g_swapped[i].table == vt) original = reinterpret_cast<PresentFn>(g_swapped[i].original[8]);
    if (!original && vt[8] != reinterpret_cast<void *>(&hk_present)) original = reinterpret_cast<PresentFn>(vt[8]);
    if (!original) {  // our slot in a vtable we do not know: nothing to call
        log::write("overlay: hk_present on %p with an unknown vtable %p", sc, vt);
        InterlockedDecrement(&g_present_inflight);
        return DXGI_ERROR_INVALID_CALL;
    }
    if (!g_stop_draw && InterlockedExchange(&g_armed, 0) && !(flags & DXGI_PRESENT_TEST)) renderer::frame(sc);
    HRESULT hr = original(sc, sync, flags);
    if (++g_presents == 1)
        log::write("overlay: first Present through hk_present, sync %u flags %x -> hr %08lx", sync, flags,
                   static_cast<unsigned long>(hr));
    InterlockedDecrement(&g_present_inflight);
    return hr;
}

// Copied: every leading slot that points at code, since the class may keep virtuals past the COM interface (a
// destructor reached from Release goes through the copy).
void swap_in(void *obj) {
    void **vt = *static_cast<void ***>(obj);
    if (vt == g_refused) return;
    for (int i = 0; i < g_count; i++)
        if (g_swapped[i].table == vt) return;
    for (int i = 0; i < g_count; i++)
        if (g_swapped[i].original == vt) {  // a new object of a class seen before (a frame-generation toggle)
            g_swapped[i].object = obj;
            InterlockedExchangePointer(static_cast<PVOID volatile *>(obj), g_swapped[i].table);
            log::write("overlay: swapchain %p: vtable %p, its copy %p reused", obj, vt, g_swapped[i].table);
            return;
        }
    if (g_count == kMaxClasses) {
        static bool said;
        if (!said) log::write("overlay: %d vtables copied already, swapchain %p left alone", kMaxClasses, obj);
        said = true;
        return;
    }
    int n = 0;
    while (n < kMaxSlots && mem::readable(&vt[n], sizeof(void *)) && mem::is_code(vt[n])) n++;
    if (n < kSwapChain4Slots || !mem::readable(&vt[-1], sizeof(void *))) {
        log::write("overlay: swapchain %p vtable %p has %d code slots, fewer than IDXGISwapChain4's %d - not swapped",
                   obj, vt, n, kSwapChain4Slots);
        g_refused = vt;
        return;
    }
    auto **block = static_cast<void **>(HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(n + 1) * sizeof(void *)));
    if (!block) {
        log::write("overlay: HeapAlloc for %d slots failed", n + 1);
        return;
    }
    block[0] = vt[-1];  // MSVC's RTTI locator sits before the table; typeid and dynamic_cast read it
    for (int i = 0; i < n; i++) block[1 + i] = vt[i];
    void **table = block + 1;
    table[8] = reinterpret_cast<void *>(&hk_present);
    g_swapped[g_count++] = {obj, vt, table, block, n};
    InterlockedExchangePointer(static_cast<PVOID volatile *>(obj), table);
    char own[160];
    mem::owner_of(vt[n - 1], own, sizeof own);
    log::write("overlay: swapchain %p: vtable %p copied to %p, %d code slots (the last in %s), Present -> hk_present",
               obj, vt, table, n, own);
}

// On the render thread, where the object is alive and nobody else presents: every copy's slot 8 goes back to the
// original, so an older object the game kept alive presents straight to its owner, and the current object goes back
// on its own vtable. The copies stay allocated: such an object still points at one.
void restore(void *sc) {
    bool back = false;
    for (int i = 0; i < g_count; i++) g_swapped[i].table[8] = g_swapped[i].original[8];
    if (sc && mem::readable(sc, sizeof(void *))) {
        void **vt = *static_cast<void ***>(sc);
        for (int i = 0; i < g_count && !back; i++)
            if (g_swapped[i].table == vt) {
                InterlockedExchangePointer(static_cast<PVOID volatile *>(sc), g_swapped[i].original);
                back = true;
            }
    }
    InterlockedExchange(&g_armed, 0);
    log::write("overlay: restore on the render thread: swapchain %p %s, slot 8 of %d copied vtable(s) put back", sc,
               back ? "back on its vtable" : "was not ours", g_count);
    InterlockedExchange(&g_restore_done, 1);
}

}  // namespace

void on_wrapper(void *swapchain, void *device, void *queue) {
    if (g_restore_requested) {
        if (!g_restore_done) restore(swapchain);
        return;
    }
    if (g_stop_draw || !swapchain || !device || !queue) return;
    renderer::set_target(static_cast<ID3D12Device *>(device), static_cast<ID3D12CommandQueue *>(queue));
    swap_in(swapchain);
    InterlockedExchange(&g_armed, 1);
}

bool unload() {
    InterlockedExchange(&g_stop_draw, 1);
    InterlockedExchange(&g_restore_requested, 1);
    for (int t = 0; t < 50 && !g_restore_done; t++) Sleep(100);
    if (!g_restore_done) {
        log::write("overlay: the render thread did not come by in 5 s - staying mapped");
        return false;
    }
    // a Present that entered hk_present before the restore can sit in the original for a frame or more (vsync)
    for (int waited = 0;;) {
        Sleep(200);
        waited += 200;
        if (g_present_inflight == 0) break;
        if (waited == 2000)
            log::write("overlay: %ld Present call(s) still in hk_present after 2 s, waiting",
                       static_cast<long>(g_present_inflight));
    }
    if (renderer::ready() && !renderer::teardown(2000)) {
        log::write("overlay: GPU still on our last list after 2 s - staying mapped");
        return false;
    }
    log::write("overlay: torn down after %ld frames drawn, %ld skipped", renderer::frames_drawn(),
               renderer::frames_skipped());
    return true;
}

}  // namespace crsrt::overlay
