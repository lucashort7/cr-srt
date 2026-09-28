// runner.h — the hook on heron::healing::regeneration_update::system, an ECS system the game calls once per tick with
// the world holder (*arg0 = the world). It is where cr-srt reads the game: inside a system call the world is stable.
// It is not called while the game is paused (pause menu, map, Death screen, main menu, window unfocused), so its call
// count going still is also the HUD's "the game is not running" signal.
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::runner {

// MinHook must be initialised. False with the reason logged.
bool install(HMODULE exe, uintptr_t runner_offset);

// The player's health as the runner last published it: true = resolved, *frac of *max points; false = not.
// *calls is the runner's call count either way. Any thread.
bool health(float *frac, int *max, long *calls);

// Threads inside the hook right now; the unloader waits for 0 before unmapping.
long inflight();

}  // namespace crsrt::runner
