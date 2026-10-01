// guarded.h — a copy that faults safely: rep movsb with a vectored exception handler that turns an access violation on
// that one instruction into a false return. It costs what memcpy costs, where ReadProcessMemory on this process waited
// 1-6.5 ms on the game's thread for the same bytes memcpy read in 0-3 µs (2026-10-01, the stutter card), since the
// syscall queues on the process's address-space lock the game's allocator holds while it spawns.
#pragma once

#include <cstddef>

namespace crsrt::guarded {

// Adds the handler, first in line. Before any copy; again is a no-op. False when Windows refuses.
bool install();

// Removes the handler and waits for every call of it already running. After every thread that copies has stopped,
// before the DLL is unmapped.
void remove();

// The handler is in place: copy() is safe to call.
bool installed();

// Copies n bytes at src to dst. False when a byte of src is not readable (an access violation, or a guard page,
// which the fault then clears); dst is then partly written. Without install(), a bad src kills the process.
bool copy(void *dst, const void *src, size_t n);

}  // namespace crsrt::guarded
