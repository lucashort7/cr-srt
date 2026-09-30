// player.h — the player's entity, through the game's own get_player, and its rows: Health, the transform, Movement.
#pragma once

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
    uint8_t logic[2064] = {};
    float h = 0.0f, total = 0.0f;  // its length on the ground plane (x-z), and its whole length
    // Where each value was read, for the log and a Cheat Engine check: 0 = that row was not read.
    int64_t arch = -1;
    uint64_t health_at = 0, position_at = 0, speed_at = 0, velocity_at = 0;
};

// The player's Health, position, speed and velocity: row 0 of the chunk get_player names. has_health false leaves every
// flag false. Cheap enough for every runner call: a repeat chunk skips the archetype scan, and one VirtualQuery covers
// the four rows. Runner thread only.
void read(void *world, State &out);

// The player's Health row (+0 the fraction of max as a float, +4 max health in points as an int), or nullptr when
// anything on the way is unreadable. Shares read's resolution. Runner thread only.
const float *health_row(void *world, int64_t *arch_out, uint64_t *chunk_out);

// get_player's answer and the player's whole archetype, to the log. Once, on the first runner call.
void log_first_look(void *world);

}  // namespace crsrt::player
