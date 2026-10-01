// present.cpp
#include "hooks/present.h"

#include "game.h"
#include "hooks/registry.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"
#include "overlay/overlay.h"

#include <dxgi.h>

#include <cstdio>

namespace crsrt::present {

namespace {

// No stack arguments and no HRESULT back: eax at the wrapper's ret is whatever its last call left.
using WrapperFn = uint64_t (*)(void *, void *, void *, void *);

hooks::Hook g_hook("present_wrapper");
HMODULE g_exe;
volatile LONG g_calls;

// What the last log line described, so a line is written only when the swapchain, its size or its format moves.
void *g_last_sc, *g_last_engine_sc;
DXGI_SWAP_CHAIN_DESC g_last_desc;

void *direct_queue() {
    auto **global = reinterpret_cast<uint8_t **>(reinterpret_cast<uint8_t *>(g_exe) + game::kDirectQueueGlobal);
    return mem::readable(global, 8) && mem::readable(*global, 0x18) ? *reinterpret_cast<void **>(*global + 0x10)
                                                                    : nullptr;
}

void *device() {
    auto **global = reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(g_exe) + game::kDeviceGlobal);
    return mem::readable(global, 8) ? *global : nullptr;
}

// The IDXGISwapChain the wrapper is about to present, or nullptr while the game has none.
void *swapchain_of(void *self) {
    if (!mem::readable(self, 8)) return nullptr;
    auto *engine_sc = *static_cast<uint8_t **>(self);
    if (!mem::readable(engine_sc + 8, 8)) return nullptr;
    void *sc = *reinterpret_cast<void **>(engine_sc + 8);
    return mem::readable(sc, 8) && mem::readable(*static_cast<void **>(sc), 0x48) ? sc : nullptr;  // through slot 8
}

// The first 16 bytes at p and its owner, and where a leading jmp goes: another hooker's inline patch shows here.
void dump16(const char *what, const uint8_t *p) {
    if (!mem::readable(p, 16)) {
        log::write("%s %p unreadable", what, p);
        return;
    }
    char own[160], to[160] = "", tail[200] = "";
    mem::owner_of(p, own, sizeof own);
    const uint8_t *target = nullptr;
    if (p[0] == 0xe9) {
        target = p + 5 + *reinterpret_cast<const int32_t *>(p + 1);
    } else if (p[0] == 0xff && p[1] == 0x25) {
        const uint8_t *slot = p + 6 + *reinterpret_cast<const int32_t *>(p + 2);
        if (mem::readable(slot, 8)) target = *reinterpret_cast<const uint8_t *const *>(slot);
    }
    if (target) {
        mem::owner_of(target, to, sizeof to);
        wsprintfA(tail, " -> jmp %p (%s)", target, to);
    }
    log::write("%s %p (%s): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x%s", what, p,
               own, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14],
               p[15], tail);
}

// Logs the swapchain the first time, whenever it or the engine's object changes, and (checked every ~5 s) whenever
// its size, format or buffer count changes.
void describe(void *self, LONG n) {
    if (!mem::readable(self, 8)) return;
    auto *engine_sc = *static_cast<uint8_t **>(self);
    if (!mem::readable(engine_sc, 0x70)) return;
    auto *sc = *reinterpret_cast<IDXGISwapChain **>(engine_sc + 8);
    bool moved = sc != g_last_sc || engine_sc != g_last_engine_sc;
    if (!moved && n % 300 != 0) return;
    if (!mem::readable(sc, 8) || !mem::readable(*reinterpret_cast<void **>(sc), 0x68)) {  // through GetDesc, slot 12
        if (moved)
            log::write("present #%ld: d3d::SwapChain %p, swapchain %p unreadable", static_cast<long>(n), engine_sc, sc);
        g_last_sc = sc;
        g_last_engine_sc = engine_sc;
        return;
    }
    DXGI_SWAP_CHAIN_DESC d{};
    HRESULT hr = sc->GetDesc(&d);
    bool resized = d.BufferDesc.Width != g_last_desc.BufferDesc.Width ||
                   d.BufferDesc.Height != g_last_desc.BufferDesc.Height ||
                   d.BufferDesc.Format != g_last_desc.BufferDesc.Format || d.BufferCount != g_last_desc.BufferCount;
    if (!moved && !resized) return;
    void **vt = *reinterpret_cast<void ***>(sc);
    char own_vt[160], own_present[160];
    mem::owner_of(vt, own_vt, sizeof own_vt);
    mem::owner_of(vt[8], own_present, sizeof own_present);
    log::write(
        "present #%ld tid %lu: self %p d3d::SwapChain %p hwnd %p swapchain %p ffx %p  +62=%u +64=%u +65=%u +68=%u",
        static_cast<long>(n), GetCurrentThreadId(), self, engine_sc, *reinterpret_cast<void **>(engine_sc), sc,
        *reinterpret_cast<void **>(engine_sc + 0x10), engine_sc[0x62], engine_sc[0x64], engine_sc[0x65],
        engine_sc[0x68]);
    log::write("  vtable %p (%s), Present %p (%s)", vt, own_vt, vt[8], own_present);
    log::write("  GetDesc hr %08lx: %ux%u format %d buffers %u swapeffect %d flags %x windowed %d",
               static_cast<unsigned long>(hr), d.BufferDesc.Width, d.BufferDesc.Height,
               static_cast<int>(d.BufferDesc.Format), d.BufferCount, static_cast<int>(d.SwapEffect), d.Flags,
               d.Windowed);
    void *queue = direct_queue(), *dev = device();
    char own_q[160] = "-", own_d[160] = "-";
    if (queue && mem::readable(queue, 8)) mem::owner_of(*static_cast<void **>(queue), own_q, sizeof own_q);
    if (dev && mem::readable(dev, 8)) mem::owner_of(*static_cast<void **>(dev), own_d, sizeof own_d);
    log::write("  queue %p (vtable in %s) device %p (vtable in %s)", queue, own_q, dev, own_d);
    if (moved) dump16("  Present bytes", static_cast<const uint8_t *>(vt[8]));
    g_last_sc = sc;
    g_last_engine_sc = engine_sc;
    g_last_desc = d;
}

uint64_t hk_wrapper(void *self, void *b, void *c, void *d) {
    hooks::Call call(g_hook);
    LONG n = InterlockedIncrement(&g_calls);
    if (!call.stopping()) {
        describe(self, n);
        if (n % 3600 == 0) log::write("present: %ld calls", static_cast<long>(n));  // ~every minute: still alive
    }
    overlay::on_wrapper(swapchain_of(self), device(), direct_queue());  // also while stopping: the restore is here
    call.before_original();
    const uint64_t r = g_hook.original<WrapperFn>()(self, b, c, d);
    if (n == 1) log::write("present: first call back, rax %p", reinterpret_cast<void *>(r));
    return r;
}

}  // namespace

bool install(HMODULE exe) {
    g_exe = exe;
    const auto *wrapper = reinterpret_cast<const uint8_t *>(exe) + game::kPresentWrapper;
    char what[48];
    std::snprintf(what, sizeof what, "capture: wrapper exe+%lx", static_cast<unsigned long>(game::kPresentWrapper));
    dump16(what, wrapper);
    if (!mem::readable(wrapper, sizeof game::kPresentWrapperPrologue)) return false;
    for (size_t i = 0; i < sizeof game::kPresentWrapperPrologue; i++)
        if (wrapper[i] != game::kPresentWrapperPrologue[i]) {
            log::write("capture: prologue differs at byte %d - not hooked", static_cast<int>(i));
            return false;
        }
    return g_hook.install(const_cast<uint8_t *>(wrapper), reinterpret_cast<void *>(&hk_wrapper));
}

}  // namespace crsrt::present
