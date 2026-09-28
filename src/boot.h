// boot.h — the start both DLLs share: wait for the exe, then the offsets they hook, from the cache or solved.
#pragma once

#include <windows.h>

#include "cache.h"

namespace crsrt::boot {

// The game's module, waited for up to 5 s; nullptr when it never appears or the unloader is already stopping.
HMODULE wait_for_exe();

// The exe offsets from the cache, or solved by pattern and cached. A [solve] getplayer=<hex> in <basename>.ini is the
// fallback while the get_player scan is young. False with the reason logged.
bool offsets_for(HMODULE exe, cache::Offsets &o);

}  // namespace crsrt::boot
