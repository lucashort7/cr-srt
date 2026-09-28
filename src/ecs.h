// ecs.h — reading the game's ECS world: which archetype a chunk belongs to, and where a component's row sits in it.
// An entity lives in a chunk of its archetype; it changes archetype, and often chunk, whenever a component is added
// to it or removed, so nothing here is worth more than the call that read it. Layout constants: game.h.
#pragma once

#include <cstdint>

namespace crsrt::ecs {

struct Archetype {
    int64_t index = -1;
    const uint32_t *hashes = nullptr;   // component type hash per type index
    const uint32_t *offsets = nullptr;  // row offset from the chunk per type index
    uint32_t count = 0;
};

// The archetype whose chunk-table slot holds `chunk`, or -1. Reads the table a memory region at a time.
int64_t archetype_of_chunk(const uint8_t *world, uint64_t chunk);

// True when chunk-table slot `index` still holds `chunk`: a cheap check that a remembered archetype is current.
bool chunk_is_at(const uint8_t *world, int64_t index, uint64_t chunk);

// The archetype's type and offset tables, checked readable. False when either is not.
bool archetype(const uint8_t *world, int64_t index, Archetype &out);

// The type's index in the archetype, or -1.
int type_index(const Archetype &archetype, uint32_t hash);

// Logs an archetype's every type hash and offset, and the rows of the types game.h names. For the first runner call.
void log_archetype(const uint8_t *world, uint64_t chunk, uint64_t tag_row);

}  // namespace crsrt::ecs
