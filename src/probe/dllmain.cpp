// dllmain.cpp — cr-probe's entry: the runner hook with a snapshot on F9 (probe/snapshot.h), nothing drawn. Injected
// alone, never beside cr-srt: both hook the runner, and MinHook chains the second over the first's jump, which the
// first unload would then tear out from under it.
#include <windows.h>

#include "boot.h"
#include "cache.h"
#include "game.h"
#include "hooks/runner.h"
#include "lifecycle.h"
#include "log.h"
#include "player.h"
#include "probe/snapshot.h"
#include "unload.h"

#include "MinHook.h"

namespace {

using namespace crsrt;

DWORD WINAPI boot_main(LPVOID) {
    HMODULE exe = boot::wait_for_exe();
    if (!exe) {
        log::write("boot: %ls not found - nothing hooked", game::kExeName);
        return 0;
    }
    MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK) {
        log::write("boot: MH_Initialize %d - nothing hooked", static_cast<int>(ms));
        return 0;
    }
    lifecycle::minhook_ready = true;

    cache::Offsets o;
    if (lifecycle::stopping || !boot::offsets_for(exe, o)) return 0;
    player::set_get_player(reinterpret_cast<player::GetPlayerFn>(reinterpret_cast<uint8_t *>(exe) + o.get_player));
    runner::set_tick(&snapshot::tick);
    lifecycle::runner_hooked = runner::install(exe, o.runner);
    if (lifecycle::runner_hooked)
        log::write("probe: F9 in game, or a %ls file, takes a snapshot", log::path(L".dump").c_str());
    return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        log::init(self);
        log::write("cr-probe attached pid=%lu tick=%lu ms", GetCurrentProcessId(), GetTickCount());
        HANDLE boot_thread = CreateThread(nullptr, 0, boot_main, nullptr, 0, nullptr);
        unload::start(self, boot_thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        log::write("cr-probe detached pid=%lu", GetCurrentProcessId());
    }
    return TRUE;
}
