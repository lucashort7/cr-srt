// source.h — where game memory is read from. The ECS readers (ecs, the enemy walk, the player, the probe's snapshot)
// take a Source and only copy out of it: never a pointer to follow, so a row freed mid-read fails the copy instead of
// faulting, and a test hands them a snapshot file where the game hands them the live process.
#pragma once

#include <cstddef>
#include <cstdint>

namespace crsrt::mem {

class Source {
   public:
    virtual ~Source() = default;
    // Copies the n bytes at the game address `at` into out. False when any of them is not there; out is then partly
    // written and not to be read.
    virtual bool copy(uint64_t at, void *out, size_t n) const = 0;
};

// This process, read with ReadProcessMemory: a page freed or never mapped fails the copy, and no VirtualQuery is asked.
const Source &live();

// One value of T at `at`.
template <class T>
bool read(const Source &source, uint64_t at, T &out) {
    return source.copy(at, &out, sizeof out);
}

}  // namespace crsrt::mem
