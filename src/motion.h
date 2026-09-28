// motion.h — the player's velocity derived from its position between runner calls, beside the game's own speed: a
// probe of what Movement+0x20 measures (a climb included or not) and of the peaks the 20 s player line misses, a jump,
// a dash, a fall. Reads nothing of the game's: only what player::read published.
#pragma once

#include "player.h"

#include <cstddef>

namespace crsrt::motion {

// One runner call's reading. Logs a line for each second that held a peak, and each teleport. Runner thread only.
void sample(const player::State &s, long calls);

// "calc h 7.48 v 0" for the 20 s player line: the last derived velocity, horizontal and vertical, in metres a second;
// "calc --" when none. Runner thread only.
void text(char *out, size_t cap);

}  // namespace crsrt::motion
