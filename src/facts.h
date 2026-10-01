// facts.h — the game's FactDictionary read by key: the open-addressed hash table every scripted flag and number of the
// story lives in, the Source balance among them (AKTH-0113, 2026-09-30). The dictionary is the env hud_visibility's
// tray holds in slot 3; its layout is in game.h (kFactTable..). Nothing here writes to the game.
#pragma once

#include <cstdint>

namespace crsrt::facts {

// The double stored under `key`, false when the entry is absent, not a double, or the table unreadable. Any thread;
// every pointer is re-read on each call, since the table reallocates when it grows and the dictionary is per world.
bool read_double(const uint8_t *dictionary, uint64_t key, double *out);

}  // namespace crsrt::facts
