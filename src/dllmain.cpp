// dllmain.cpp — cr-srt's entry. DllMain runs under the loader lock, so it only records where the DLL lives and starts
// two threads: the boot, which finds and hooks what cr-srt needs, and the unloader (unload.h).
#include <windows.h>

#include "boot.h"
#include "cache.h"
#include "ecs.h"
#include "enemies.h"
#include "game.h"
#include "hooks/damage.h"
#include "hooks/present.h"
#include "hooks/runner.h"
#include "hooks/script.h"
#include "hooks/sonar.h"
#include "hooks/transit.h"
#include "hooks/uistate.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"
#include "player.h"
#include "solve.h"
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
    if (!lifecycle::stopping && boot::offsets_for(exe, o)) {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(exe) + o.get_player;
        if (mem::readable(p, 8))
            log::write("getplayer exe+%lx = %p bytes %02x %02x %02x %02x %02x %02x %02x %02x",
                       static_cast<unsigned long>(o.get_player), p, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        player::set_get_player(reinterpret_cast<player::GetPlayerFn>(const_cast<uint8_t *>(p)));
        ecs::set_find_type(o.find_type);
        if (!lifecycle::stopping) lifecycle::runner_hooked = runner::install(exe, o.runner);
        // the walk on its own thread, off what the runner publishes; nothing runs on the runner's call for it
        if (!lifecycle::stopping && lifecycle::runner_hooked) enemies::start();
        if (!lifecycle::stopping && o.sonar) sonar::install(exe, o.sonar);
        if (!lifecycle::stopping && o.hudvis) uistate::install(exe, o.hudvis);
        if (!lifecycle::stopping && o.pausedet) uistate::install_pause(exe, o.pausedet);
        if (!lifecycle::stopping && o.challenge) uistate::install_challenge(exe, o.challenge);
        // the probe after the runner, whose world it reads
        if (!lifecycle::stopping && GetPrivateProfileIntW(L"probe", L"damage", 0, log::path(L".ini").c_str()))
            damage::install(exe);
    }
    if (!lifecycle::stopping) lifecycle::present_hooked = present::install(exe);
    // the taxi probe last: nine solves the overlay need not wait for
    if (!lifecycle::stopping && GetPrivateProfileIntW(L"probe", L"transit", 1, log::path(L".ini").c_str()))
        transit::install(exe);
    if (!lifecycle::stopping && GetPrivateProfileIntW(L"probe", L"script", 1, log::path(L".ini").c_str()))
        script::install(exe);
    return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        log::init(self);
        log::write("cr-srt attached pid=%lu tick=%lu ms", GetCurrentProcessId(), GetTickCount());
        HANDLE boot_thread = CreateThread(nullptr, 0, boot_main, nullptr, 0, nullptr);
        unload::start(self, boot_thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        log::write("cr-srt detached pid=%lu", GetCurrentProcessId());
    }
    return TRUE;
}
