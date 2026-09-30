// cache.h — the exe offsets cr-srt solved once, kept in <basename>.cache as one line keyed by a fingerprint of the exe,
// the way crheal keeps its own. Same exe, same fingerprint: the offsets are trusted. A patch changes the
// fingerprint, the line is thrown away and solved again (solve.h).
#pragma once

#include <windows.h>

#include <cstdint>

namespace crsrt::cache {

struct Offsets {
    uintptr_t get_player = 0;
    uintptr_t get_player_lookup = 0;  // where the scan found get_player's type hash; kept for the record
    uintptr_t runner = 0;
    uintptr_t sonar = 0;     // optional: 0 when its solve failed, and cr-srt runs without the district
    uintptr_t hudvis = 0;    // optional: heron::hud_visibility::system, the state probe (AKTH-0110)
    uintptr_t pausedet = 0;  // optional: sndencore::pause_state_detection::system, the same probe
};

enum class Status { Absent, BadVersion, BadFingerprint, Hit };

// FNV-1a 64 of TimeDateStamp, SizeOfImage and CheckSum, read from the loaded headers: any rebuild changes one.
uint64_t fingerprint(HMODULE exe);

// Hit only when the version, the fingerprint and every key the line must carry all match.
Status load(uint64_t fp, Offsets &out);
void save(uint64_t fp, const Offsets &offsets);

const char *describe(Status status);

}  // namespace crsrt::cache
