// uistate.h — the game's own word for its UI state, read off two systems hooked by name (game.h) and changing
// nothing in the game. sndencore::pause_state_detection::system reads AudioPause, whose byte +0 is 1 while any
// screen of the hub (Tab, M, C, I, J, L) or the pause menu (Esc) is open and 0 in the world: 7 of 7 on 2026-09-30,
// per frame, while the runner and the sonar are not called at all. heron::hud_visibility::system reads
// UIStateStacks, whose bitsets at +0x128..+0x138 name the screen's content (the map, the inventory) and are logged
// on change as a lead (AKTH-0110). Both run at frame rate, in and out of a pause.
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::uistate {

// MinHook must be initialised. False with the reason logged.
bool install(HMODULE exe, uintptr_t hud_visibility_offset);
bool install_pause(HMODULE exe, uintptr_t pause_detection_offset);

// AudioPause's flag as the last call read it: a hub screen or the pause menu is open. Any thread.
bool paused();

// Threads inside either hook right now; the unloader waits for 0 before unmapping.
long inflight();

}  // namespace crsrt::uistate
