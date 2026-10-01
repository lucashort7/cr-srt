// script.h — a probe for what starts a taxi ride (AKTH-0110): C functions the game registers for its Luau scripts,
// hooked by name (solve::script_function) and changing nothing. Each call logs its arguments off the Luau stack, the
// first kLogged calls of each in full, then one line per 100. The candidates are the return point and level switch
// (slot-0_return is rewritten at a taxi's entry), the jump point and teleport, the Gap and the challenge. Off with
// [probe] script=0 in the ini.
#pragma once

#include <windows.h>

namespace crsrt::script {

// Solves and hooks each function on the boot thread, after the transit probe; stops between solves once the unloader
// has started. The count hooked; MinHook must be initialised.
int install(HMODULE exe);

// Threads inside any of these hooks right now; the unloader waits for 0 before unmapping.
long inflight();

}  // namespace crsrt::script
