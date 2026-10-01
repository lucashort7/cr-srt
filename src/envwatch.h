// envwatch.h — a block of the game's memory diffed by qword against its last reading, the changes logged as
// "<who>: <name> diff +off old>new". A qword that changes on kMuteRun samples in a row is a clock, and one that has
// changed kMuteTotal times is a counter: either is muted, said once, and its later changes are kept but not logged. A
// flag that flips and rests is never muted. Shared by the state probes (AKTH-0110).
#pragma once

#include <cstdint>

namespace crsrt::envwatch {

inline constexpr int kMaxBytes = 512;
inline constexpr uint8_t kMuteRun = 24;
inline constexpr uint8_t kMuteTotal = 200;

// One qword's history: changes on consecutive samples, changes in all; run == kMuted once muted.
struct Count {
    uint8_t run = 0;
    uint8_t total = 0;
};
inline constexpr uint8_t kMuted = 0xff;

// `now` against `seen`, both `size` bytes (a multiple of 8), then `now` copied into `seen`. `counts` holds one Count
// per qword, zeroed before the first call. Any thread; the caller owns the three buffers.
void diff_bytes(const char *who, const char *name, const uint8_t *now, uint8_t *seen, Count *counts, int size,
                long call);

// Records the env pointers of a system's tray, `slots` of them, for the cap below. Any thread; the first call of each
// hooked system notes its tray (runner::log_env_tray), so every tray the probes have seen bounds every watch.
void note_tray(const uint8_t *tray, int slots);

// One env of a system's tray. `slot` is its byte offset in the tray (index * 8), as game.h writes it. `follow`: the
// slot's object holds a pointer (an r::Ptr) and what it points at is diffed. `size` is asked, at most kMaxBytes; each
// reading caps it at the nearest env any tray has shown above this one, because the arena packs envs and a watch past
// its own reads the neighbour (the FactDictionary lesson, and TimelineState over UIVisibility on 2026-09-30).
struct Watch {
    const char *name;
    uintptr_t slot;
    bool follow;
    int size;
    const uint8_t *at = nullptr;  // the object last read, logged when it moves
    int cap = 0;
    bool ever = false;
    bool said_empty = false;
    bool said_unreadable = false;
    uint8_t seen[kMaxBytes] = {};
    Count counts[kMaxBytes / 8] = {};
};

// Reads `w` off `tray`, whose `slots` pointers are the system's parameters, and logs what moved. The first reading
// logs the address and the bytes watched; an empty slot is said once.
void read(Watch &w, const uint8_t *tray, int slots, const char *who, long call);

}  // namespace crsrt::envwatch
