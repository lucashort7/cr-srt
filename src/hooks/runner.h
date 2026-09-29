// runner.h — the hook on heron::healing::regeneration_update::system, an ECS system the game calls once per tick with
// the world holder (*arg0 = the world). It is where cr-srt reads the game: inside a system call the world is stable.
// It is not called while the game is paused (pause menu, map, Death screen, main menu, window unfocused), so its call
// count going still is also the HUD's "the game is not running" signal.
#pragma once

#include "player.h"

#include <windows.h>

#include <cstdint>

namespace crsrt::runner {

// MinHook must be initialised. False with the reason logged.
bool install(HMODULE exe, uintptr_t runner_offset);

// The player as the runner last read it (every flag false before the first read), and the runner's call count in
// *calls either way. Any thread.
void player_state(player::State &out, long *calls);

// The world the runner saw on its last call, or nullptr before the first. Any thread; only as good as that call.
void *world();

// Called by the hook on every call, before the game's system runs, with the world and the call count: the place to read
// the ECS where it is stable. Null (the default) calls nothing. Set it before install().
using Tick = void (*)(void *world, long calls);
void set_tick(Tick tick);

// Threads inside the hook right now; the unloader waits for 0 before unmapping.
long inflight();

// Logs a system's env tray (game.h, kEnvTray) off a runner's third argument: each slot's pointer, its owner and its
// first 16 bytes as four floats and as hex. For checking which slot holds which parameter.
void log_env_tray(const char *who, void *ctx, int slots);

}  // namespace crsrt::runner
