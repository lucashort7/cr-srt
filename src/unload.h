// unload.h — taking cr-srt out of a running game. The trigger is a file, <basename>.unload beside the DLL; the DLL
// then undoes its boot in reverse order and unmaps itself from a thread of its own. Ejecting it from outside instead
// would unmap code under a live hook and crash the game.
#pragma once

#include <windows.h>

namespace crsrt::unload {

// Starts the thread that watches for the flag. `boot` is the boot thread, waited for before anything is undone.
void start(HMODULE self, HANDLE boot);

}  // namespace crsrt::unload
