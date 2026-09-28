// unload.cpp — cr-probe's unloader: cr-srt's (src/unload.cpp) with the runner as the only hook. Every exit that is not
// the last line leaves the DLL mapped, for the same reason as there.
#include "unload.h"

#include "hooks/runner.h"
#include "lifecycle.h"
#include "log.h"

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
    if (lifecycle::runner_hooked) {
        MH_STATUS ms = MH_DisableHook(MH_ALL_HOOKS);
        for (int t = 0; ms != MH_OK && t < 10; t++) {
            Sleep(100);
            ms = MH_DisableHook(MH_ALL_HOOKS);
        }
        log::write("unload: runner hook %s", ms == MH_OK ? "restored" : "disable FAILED - staying mapped");
        if (ms != MH_OK) ExitThread(0);
        int waited = 0;
        do {
            Sleep(200);
            waited += 200;
        } while (runner::inflight() != 0);
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
