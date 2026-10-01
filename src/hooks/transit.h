// transit.h — a probe for the taxi (AKTH-0110): the game's transition systems hooked by name (game.h), changing
// nothing in the game. Each logs its first call and its tray, a resume after a second of silence (a system gated to a
// transition speaks only then), its call count every 20 s, and its envs diffed at most every 100 ms and on every
// resume (envwatch.h). Off with [probe] transit=0 in the ini.
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::transit {

// Solves and hooks each system, on the boot thread after the main hooks: not cached, a solve is 50-600 ms and a probe
// does not earn a cache line. Stops between solves once the unloader has started. The count hooked; MinHook must be
// initialised.
int install(HMODULE exe);

// Threads inside any of these hooks right now; the unloader waits for 0 before unmapping.
long inflight();

// GameState+0x90: the 64-bit id of the level loaded, a zone, a building or a sequence, as the transition system last
// ran (2026-10-01: ZONE_HISS 01c080962a7d636d, a tower inside it 184dcbbd71e5463d, unchanged across its floors). 0
// before the first call. Any thread.
uint64_t area();

}  // namespace crsrt::transit
