// damage.h — a probe: the hook on the game's damage function (game.h, kTakeDamage), which every entity's hit goes
// through, the player's and each enemy's. Each hit is logged with the archetype its Health row lives in, and each new
// archetype's component list once, to find which component tells one kind of enemy from another. It changes nothing
// in the game. Opt-in, since it only serves a discovery: [probe] damage=1 in <basename>.ini.
#pragma once

#include <windows.h>

namespace crsrt::damage {

// MinHook must be initialised. False (nothing hooked) when the function's first bytes are not the ones game.h expects.
bool install(HMODULE exe);

}  // namespace crsrt::damage
