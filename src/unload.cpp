// unload.cpp — every exit below that is not the last line leaves the DLL mapped: unmapping code a thread can still
// return into kills the game, and a DLL left mapped costs nothing.
#include "unload.h"

#include "enemies.h"
#include "guarded.h"
#include "hooks/registry.h"
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
    // The walk thread first: it is ours, it reads the world on its own, and its code must not be unmapped under it. One
    // that does not end keeps the DLL mapped, but the overlay and the hooks still go: the game is left as it was, less
    // a thread that only reads.
    const bool walk_stopped = enemies::stop();
    // the overlay next, while the Present hook still runs: the swapchain's vtable goes back on the render thread
    if (lifecycle::present_hooked && !overlay::unload()) ExitThread(0);

    if (!hooks::release()) ExitThread(0);
    if (!walk_stopped) {
        log::write("unload: hooks and overlay gone, the walk thread still running - staying mapped");
        ExitThread(0);
    }
    guarded::remove();  // no thread of ours copies any more
    if (lifecycle::minhook_ready) {
        MH_STATUS mu = MH_Uninitialize();
        if (mu != MH_OK && mu != MH_ERROR_NOT_INITIALIZED) {
            log::write("unload: MH_Uninitialize %d - staying mapped", static_cast<int>(mu));
            ExitThread(0);
        }
    }
    log::write("unload: detaching pid=%lu", GetCurrentProcessId());
    log::shutdown();  // the writer thread must be gone before the code it runs is unmapped
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
