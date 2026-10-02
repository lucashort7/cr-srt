// spawn.h — the spawn queue's drain (game.h, kSpawnBundle), hooked for good: the kind of every record it is about to
// drain goes to the dictionary (dict.h), so a kind lands even when the walk never reaches it. Around it, a probe:
// read-only hooks on the submit, the bundle, the fill of a fresh entity's rows and Health's two callbacks, logging
// their arguments, the 0x90-byte records and, with the game's column lookup (solve::find_type), which callers look up
// which columns in the frame of a bundle with records. Neither changes anything in the game. The probe is on by
// default while the path is being read: [probe] spawn=0 in <basename>.ini leaves the feed alone. Stories AKTH-0119
// (the probe) and AKTH-0120 (the feed).
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::spawn {

// MinHook must be initialised. The bundle always, the probe's hooks with `probe`; find_type is the lookup's exe offset,
// 0 when unsolved (the lookup is then not hooked). False when nothing hooked, each function checked by its first bytes.
bool install(HMODULE exe, uintptr_t find_type, bool probe);

}  // namespace crsrt::spawn
