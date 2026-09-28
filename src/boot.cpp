// boot.cpp
#include "boot.h"

#include "game.h"
#include "lifecycle.h"
#include "log.h"
#include "solve.h"

#include <cwchar>

namespace crsrt::boot {

HMODULE wait_for_exe() {
    HMODULE exe = nullptr;
    for (int tries = 0; tries < 50 && !lifecycle::stopping && !(exe = GetModuleHandleW(game::kExeName)); tries++)
        Sleep(100);
    return exe;
}

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

}  // namespace crsrt::boot
