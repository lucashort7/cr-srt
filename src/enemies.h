// enemies.h — every NPC with health, read from the ECS on each runner call: the rows of every archetype whose columns
// hold Health and SpawnedNPC, each with its shield, published nearest to the player first for the render thread.
// The route and its measurements: the enemy research note in the vault, section "The list without damage".
#pragma once

#include <cstdint>

namespace crsrt::enemies {

struct Enemy {
    uint32_t id = 0, generation = 0;  // the entity handle, which survives archetype changes
    float fraction = 0.0f;            // Health +0, of max
    int32_t max = 0;                  // Health +4, in points
    uint32_t faction = 0;             // FNV-1a 32 of the faction's lowercase name
    int32_t level = 0;
    uint8_t kind[8] = {};             // SpawnedNPC +0x30, memory order
    uint8_t tier = 0;                 // SpawnedNPC +0x3c
    float position[3] = {};
    float distance = 0.0f;            // to the player; a negative value when the player's position is unknown
    float shield_fraction = 0.0f;
    int32_t shield_max = 0;           // 0 = no shield
};

inline constexpr int kMaxRows = 64;

struct List {
    long calls = 0;  // the runner call that published it
    int total = 0;   // enemies found, past kMaxRows included
    int count = 0;   // rows filled, nearest first
    Enemy rows[kMaxRows];
};

// The runner's tick (runner::set_tick): walks, publishes, and logs the walk's cost every 600 calls. Runner thread.
void tick(void *world, long calls);

// The last published list. Any thread.
void read(List &out);

}  // namespace crsrt::enemies
