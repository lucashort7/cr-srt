// solve.h — finding the two exe functions cr-srt hooks or calls, by pattern, when the cache has nothing for this exe.
// Three rungs of fragility: a heap address dies at boot, an exe offset dies at a patch, a byte pattern survives most
// patches. These are the third rung; the cache keeps what they find.
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::solve {

// get_player's exe offset, or 0 with the reason logged. *lookup gets the offset of the site that told it apart.
uintptr_t get_player(HMODULE exe, uintptr_t *lookup);

// heron::healing::regeneration_update::system's exe offset, or 0 with the reason logged.
uintptr_t runner(HMODULE exe);

}  // namespace crsrt::solve
