// unload.cpp — every exit below that is not the last line leaves the DLL mapped: unmapping code a thread can still
// return into kills the game, and a DLL left mapped costs nothing.
#include "unload.h"

#include "enemies.h"
#include "hooks/damage.h"
#include "hooks/present.h"
#include "hooks/runner.h"
#include "lifecycle.h"
#include "log.h"
#include "overlay/overlay.h"

#include "MinHook.h"

namespace crsrt::unload {

namespace {

HMODULE g_self;
HANDLE g_boot;

DWORD WINAPI unloader(LPVOID) {
    const std::wstring flag = log::path(L".unload");
    while (GetFileAttributesW(flag.c_str()) == INVALID_FILE_ATTRIBUTES) Sleep(250);
    DeleteFileW(flag.c_str());
    log::write("unload: flag seen");
    lifecycle::stopping = true;

    if (g_boot) {
        DWORD w = WaitForSingleObject(g_boot, 5000);
        log::write("unload: boot %s", w == WAIT_OBJECT_0 ? "done" : "did not end in 5 s - staying mapped");
        CloseHandle(g_boot);
        if (w != WAIT_OBJECT_0) ExitThread(0);
    }
    // the walk thread first: it is ours, it reads the world on its own, and its code must not be unmapped under it
    if (!enemies::stop()) ExitThread(0);
    // the overlay next, while the Present hook still runs: the swapchain's vtable goes back on the render thread
    if (lifecycle::present_hooked && !overlay::unload()) ExitThread(0);

    if (lifecycle::runner_hooked || lifecycle::present_hooked || lifecycle::damage_hooked) {
        MH_STATUS ms = MH_DisableHook(MH_ALL_HOOKS);
        for (int t = 0; ms != MH_OK && t < 10; t++) {
            Sleep(100);
            ms = MH_DisableHook(MH_ALL_HOOKS);
        }
        log::write("unload: hooks %s (runner %s, wrapper %s, damage %s)",
                   ms == MH_OK ? "disabled" : "disable FAILED - staying mapped",
                   lifecycle::runner_hooked ? "restored" : "never hooked",
                   lifecycle::present_hooked ? "restored" : "never hooked",
                   lifecycle::damage_hooked ? "restored" : "never hooked");
        if (ms != MH_OK) ExitThread(0);
        // A thread that jumped in before the disable counts itself within a few instructions; the render thread can
        // then sit inside the original Present for a frame or more (vsync, a minimized window) and return into our
        // hook. With the hooks off nothing new enters, so the wait has no ceiling; it ends on a zero read after a sleep.
        int waited = 0;
        for (;;) {
            Sleep(200);
            waited += 200;
            if (present::inflight() == 0 && runner::inflight() == 0 && damage::inflight() == 0) break;
            if (waited == 2000)
                log::write("unload: %ld present / %ld runner / %ld damage call(s) still in flight after 2 s, waiting",
                           present::inflight(), runner::inflight(), damage::inflight());
        }
        log::write("unload: no call in flight after %d ms", waited);
    }
    if (lifecycle::minhook_ready) {
        MH_STATUS mu = MH_Uninitialize();
        if (mu != MH_OK && mu != MH_ERROR_NOT_INITIALIZED) {
            log::write("unload: MH_Uninitialize %d - staying mapped", static_cast<int>(mu));
            ExitThread(0);
        }
    }
    log::write("unload: detaching pid=%lu", GetCurrentProcessId());
    FreeLibraryAndExitThread(g_self, 0);
}

}  // namespace

void start(HMODULE self, HANDLE boot) {
    g_self = self;
    g_boot = boot;
    HANDLE t = CreateThread(nullptr, 0, unloader, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

}  // namespace crsrt::unload
