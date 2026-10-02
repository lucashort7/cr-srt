// enemies.h — every NPC with health, read from the ECS on a thread of ours: the rows of every archetype whose columns
// hold Health and SpawnedNPC, each with its shield, published nearest to the player first for the render thread. The
// thread walks every [enemies] interval_ms (default 100), and only when the runner was called since its last walk; the
// runner's own call stores nothing for it beyond what runner.h already publishes. A walk outside a system call reads
// while the game writes: rows that fail the health checks are rejected and counted, never published.
// The thread also runs the dictionary (dict.h): what it meets goes there, and so do the keys the hooks queue.
// The route and its measurements: the enemy research note in the vault, section "The list without damage"; the move
// off the game's thread: story AKTH-0103.
#pragma once

#include "player.h"
#include "source.h"

#include <cstdint>

namespace crsrt::enemies {

struct Enemy {
    uint32_t id = 0, generation = 0;  // the entity handle, which survives archetype changes
    float fraction = 0.0f;            // Health +0, of max
    int32_t max = 0;                  // Health +4, in points
    uint32_t faction = 0;             // FNV-1a 32 of the faction's lowercase name
    int32_t level = 0;
    uint8_t kind[8] = {};                           // SpawnedNPC +0x30, memory order
    uint8_t boss_type = 0;                          // SpawnedNPC +0x3c: 2 common, 4 elite, 6 boss
    uint32_t source_id = 0, source_generation = 0;  // SpawnedNPC +0, a handle: the spawner's, unconfirmed
    float position[3] = {};
    float distance = 0.0f;  // to the player; a negative value when the player's position is unknown
    float shield_fraction = 0.0f;
    int32_t shield_max = 0;  // 0 = no shield
};

inline constexpr int kMaxRows = 64;

struct List {
    long calls = 0;  // the runner call that published it
    int total = 0;   // rows found, past kMaxRows included, allies among them
    int allies = 0;  // those of `total` whose faction is_ally
    int count = 0;   // rows filled, nearest first
    Enemy rows[kMaxRows];
};

// Starts the walk thread, which reads runner::world and runner::player_state; before or after the runner is hooked,
// since it idles until both answer. False with the reason logged. Logs the walks' cost every 20 s.
bool start();

// Stops the thread and waits for it, at most 5 s; false = still running, the DLL must stay mapped. Never started: true.
bool stop();

// The last published list. Any thread.
void read(List &out);

// One walk of the world at `world` in `source`, as the walk thread runs it, with the player as `player` holds it: the
// list it would publish. For the tests, on a thread that is not the walk's.
void scan(const mem::Source &source, uint64_t world, const player::State &player, List &out);

}  // namespace crsrt::enemies
