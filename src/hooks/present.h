// present.h — the hook on the game's Present wrapper (game.h, kPresentWrapper): the one place, once a frame on the
// render thread, where the swapchain, the device and the Direct queue are all in reach. It logs what it sees and
// hands the swapchain to the overlay before the game presents. It draws nothing itself.
#pragma once

#include <windows.h>

namespace crsrt::present {

// MinHook must be initialised. False (nothing hooked) when the wrapper's first bytes are not the ones game.h expects.
bool install(HMODULE exe);

// Threads inside the hook right now; the unloader waits for 0 before unmapping.
long inflight();

}  // namespace crsrt::present
