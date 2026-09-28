// memory.h — questions asked of a pointer before it is followed. Every read of game memory in cr-srt goes through
// one of these first: a bad pointer is logged and skipped, it never crashes the game.
#pragma once

#include <cstddef>

namespace crsrt::mem {

// Committed, not a guard page, not no-access, and all `len` bytes inside one region.
bool readable(const void *p, size_t len);

// How many bytes are readable from p to the end of its region; 0 if p is not readable. One VirtualQuery covers a
// whole table this way instead of one per entry.
size_t readable_run(const void *p);

// Copies n bytes at p to out through ReadProcessMemory, which fails instead of faulting when another thread frees the
// memory mid-copy. For memory the game's worker threads own: chunks, type and offset tables.
bool copy(const void *p, void *out, size_t n);

// Committed and executable.
bool is_code(const void *p);

// "module.dll+offset" of the module that owns p, or "?".
void owner_of(const void *p, char *out, size_t cap);

}  // namespace crsrt::mem
