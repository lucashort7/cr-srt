// solve.h — finding the two exe functions cr-srt hooks or calls, by pattern, when the cache has nothing for this exe.
// Three rungs of fragility: a heap address dies at boot, an exe offset dies at a patch, a byte pattern survives most
// patches. These are the third rung; the cache keeps what they find.
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::solve {

// get_player's exe offset, or 0 with the reason logged. *lookup gets the offset of the site that told it apart.
uintptr_t get_player(HMODULE exe, uintptr_t *lookup);

// The exe offset of the game's find-type-index function, decoded from the call rel32 kFindTypeCallAfterLookup bytes
// past get_player's lookup site, or 0 with the reason logged.
uintptr_t find_type(HMODULE exe, uintptr_t lookup_site);

// heron::healing::regeneration_update::system's exe offset, or 0 with the reason logged.
uintptr_t runner(HMODULE exe);

// The exe offset of the system whose getName string starts with `name`, found as the runner is, or 0 with the reason
// logged. `what` names it in the log.
uintptr_t system(HMODULE exe, const char *name, const char *what);

// The exe offset of the C function the game registers for Luau under exactly `name` (nl_game_jump_to,
// return_point_save_then_switch_to_level), or 0 with the reason logged. The game builds each table of {name, function}
// pairs on the stack: lea rax, [name]; mov [rbp+d], rax; lea rax, [fn]; mov [rbp+d+8], rax. Found by that shape after
// the one lea to the name.
uintptr_t script_function(HMODULE exe, const char *name);

}  // namespace crsrt::solve
