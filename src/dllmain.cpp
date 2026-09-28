// dllmain.cpp — cr-srt's entry. DllMain runs under the loader lock, so it only records where the DLL lives and starts
// two threads: the boot, which finds and hooks what cr-srt needs, and the unloader (unload.h).
#include <windows.h>

#include "cache.h"
#include "game.h"
#include "hooks/damage.h"
#include "hooks/present.h"
#include "hooks/runner.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"
#include "player.h"
#include "solve.h"
#include "unload.h"

#include "MinHook.h"

namespace {

using namespace crsrt;

HMODULE wait_for_exe() {
    HMODULE exe = nullptr;
    for (int tries = 0; tries < 50 && !lifecycle::stopping && !(exe = GetModuleHandleW(game::kExeName)); tries++)
        Sleep(100);
    return exe;
}

// The exe offsets from the cache, or solved by pattern and cached. A [solve] getplayer=<hex> in <basename>.ini is the
// fallback while the get_player scan is young.
bool offsets_for(HMODULE exe, cache::Offsets &o) {
    uint64_t fp = cache::fingerprint(exe);
    cache::Status st = cache::load(fp, o);
    log::write("fp %08lx%08lx cache %s", static_cast<unsigned long>(fp >> 32),
               static_cast<unsigned long>(fp & 0xffffffffUL), cache::describe(st));
    if (st == cache::Status::Hit) return true;

    o = {};
    o.get_player = solve::get_player(exe, &o.get_player_lookup);
    if (!o.get_player) {
        wchar_t s[32];
        GetPrivateProfileStringW(L"solve", L"getplayer", L"", s, 32, log::path(L".ini").c_str());
        if (s[0]) {
            o.get_player = wcstoull(s, nullptr, 16);
            log::write("solved getplayer=%lx from [solve] fallback", static_cast<unsigned long>(o.get_player));
        }
    }
    o.runner = solve::runner(exe);
    if (!o.get_player || !o.runner) {
        log::write("not solved (getplayer=%lx runner=%lx) - nothing cached", static_cast<unsigned long>(o.get_player),
                   static_cast<unsigned long>(o.runner));
        return false;
    }
    cache::save(fp, o);
    log::write("solved getplayer=%lx runner=%lx - cache written", static_cast<unsigned long>(o.get_player),
               static_cast<unsigned long>(o.runner));
    return true;
}

DWORD WINAPI boot(LPVOID) {
    HMODULE exe = wait_for_exe();
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
    if (!lifecycle::stopping && offsets_for(exe, o)) {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(exe) + o.get_player;
        if (mem::readable(p, 8))
            log::write("getplayer exe+%lx = %p bytes %02x %02x %02x %02x %02x %02x %02x %02x",
                       static_cast<unsigned long>(o.get_player), p, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        player::set_get_player(reinterpret_cast<player::GetPlayerFn>(const_cast<uint8_t *>(p)));
        if (!lifecycle::stopping) lifecycle::runner_hooked = runner::install(exe, o.runner);
        // the probe after the runner, whose world it reads
        if (!lifecycle::stopping && GetPrivateProfileIntW(L"probe", L"damage", 0, log::path(L".ini").c_str()))
            lifecycle::damage_hooked = damage::install(exe);
    }
    if (!lifecycle::stopping) lifecycle::present_hooked = present::install(exe);
    return 0;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        log::init(self);
        log::write("cr-srt attached pid=%lu tick=%lu ms", GetCurrentProcessId(), GetTickCount());
        HANDLE boot_thread = CreateThread(nullptr, 0, boot, nullptr, 0, nullptr);
        unload::start(self, boot_thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        log::write("cr-srt detached pid=%lu", GetCurrentProcessId());
    }
    return TRUE;
}
