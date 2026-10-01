// player.h — the player's entity, through the game's own get_player, and its rows: Health, the transform, Movement.
#pragma once

#include "source.h"

#include <cstdint>

namespace crsrt::player {

// get_player(out, world) fills out[0] = the player tag's row, out[1] = the chunk, out[3] bit 0 = found.
using GetPlayerFn = uint64_t *(*)(uint64_t *out, void *world);

void set_get_player(GetPlayerFn fn);

// What the runner reads of the player on one call. A flag false: that row did not resolve on that call, or it held a
// value no row of its type holds (a position off the map, a negative speed). The Health range is the reader's to check,
// as before.
struct State {
    bool has_health = false, has_position = false, has_speed = false;
    float fraction = 0.0f;   // Health +0, of max
    int32_t max = 0;         // Health +4, in points
    float position[3] = {};  // WorldTransformReadOnly +0x10, Y up
    float rotation[4] = {};  // WorldTransformReadOnly +0, a quaternion x y z w: the body's orientation
    float speed = 0.0f;      // Movement +0x20, metres a second, 0 through most of a jump
    bool has_velocity = false;
    float velocity[3] = {};  // MovementState +0, metres a second, Y up
    // InGap and Status rows whole, 8 bytes each, as the log's hex; has_* false when the row did not resolve.
    bool has_gap = false, has_status = false;
    uint64_t gap = 0, status = 0;
    bool in_gap = false;     // InGap's flag byte: the player is in the Gap
    bool has_logic = false;  // LogicData row 0, whole: the story-area probe
    bool in_story = false;   // LogicData +0x42c == 2: the game's "Not While in Story Area"
    bool dead = false;       // the archetype carries heron::health::component::Dead
    // The Main Stats beside Health, and the derived ones: ResourcingData, PlayerProgression and CoreUpgradeData rows,
    // each has_* false when its row did not resolve. The floats are the game's, unchecked beyond being finite.
    bool has_power = false, has_progression = false, has_upgrades = false;
    float power = 0.0f, power_max = 0.0f;                // ResourcingData +0xc, +0x8
    float command = 0.0f, command_max = 0.0f;            // ResourcingData +0x21c, +0x214
    float command_charge = 0.0f;                         // ResourcingData +0x220, one charge's size
    int32_t command_charges = 0;                         // ResourcingData +0x224, how many charges the max holds
    int32_t build_power = 0, aberrant = 0, ability = 0;  // PlayerProgression +0, +0x14, +0x20
    int32_t health_upgrades = 0, power_upgrades = 0;     // CoreUpgradeData +0, +8
    uint8_t logic[2064] = {};
    float h = 0.0f, total = 0.0f;  // its length on the ground plane (x-z), and its whole length
    // Where each value was read, for the log and a Cheat Engine check: 0 = that row was not read.
    int64_t arch = -1;
    uint64_t health_at = 0, position_at = 0, speed_at = 0, velocity_at = 0;
};

// The player's rows: row 0 of the chunk get_player names. has_health false leaves every flag false. Cheap enough for
// every runner call: a repeat chunk skips the slot table, and each row is one copy. Runner thread only.
void read(void *world, State &out);

// read() for a chunk already known, through any source: what read() does after get_player, and what the tests run over
// a snapshot. Runner thread only, or a thread that is not the runner's while the runner is not hooked.
void read_chunk(const mem::Source &source, uint64_t world, uint64_t chunk, State &out);

// get_player's answer and the player's whole archetype, to the log. Once, on the first runner call.
void log_first_look(void *world);

}  // namespace crsrt::player
