// dict.h — the dictionary: every component type, enemy kind and faction cr-srt has met, kept in three TSV files in
// dict\ beside the DLL and read back by the next session. A key is written once, the first time it is met; later the
// DLL may fill the columns it owns while they are empty, and it never writes the name of a kind or a faction, which
// is ours (story AKTH-0120).
//
// The files: a header line naming the columns, then one row per key, tab-separated, the key first in hex.
//   types.tsv     hash  name  size  first_seen         the registry's (types()), name and size included
//   kinds.tsv     key   name  max  level  faction  b3c  shield  first_seen
//   factions.tsv  hash  name  first_seen
// A kind's key is its 8 bytes in memory order, as the log prints them; a type's and a faction's are the 32-bit hash.
// A write reads the file again and changes only the rows it adds and the empty cells it fills: a name typed by hand
// between two writes, a column of ours, a comment line, all stay as they are.
//
// Threads: known() and offer() run on any thread and take no lock to answer for a key already there; a game thread that
// meets a key the tables lack queues it, under a lock, for the walk thread (enemies.h). Everything else, the files
// included, runs on the walk thread.
#pragma once

#include "source.h"

#include <cstdint>

namespace crsrt::dict {

enum Class : uint8_t { kTypes, kKinds, kFactions };
inline constexpr int kClasses = 3;

// What the walk reads off a living enemy: the columns a kind's row owns.
struct Stats {
    int32_t max = 0;
    int32_t level = 0;
    uint32_t faction = 0;
    uint8_t b3c = 0;
    int32_t shield = 0;  // the shield's max, 0 for none
};

// --- Any thread.

// The key is in class c's table. 0 never is.
bool known(Class c, uint64_t key);

// A key met by a game thread: queued for the walk thread when the table lacks it, dropped (and counted) past the
// queue's 1024 entries. What the walk thread does with it is what kind() does without stats, for a kind.
void offer(Class c, uint64_t key);

// --- The walk thread.

// dict\ beside the DLL, made when missing, and the three files loaded. Once: a second call does nothing.
void open();
// open() on another folder, with its trailing backslash, after forgetting everything: for the tests.
void open_at(const wchar_t *folder);

// The type registry whose pointer the runner's world holds (game::kWorldTypeRegistry), read once per world: each type
// not in the table, or in it with an empty name or size, is written with what the registry says. A name that does not
// hash to its key (FNV-1a 32) is kept only when Health's record names Health. False when the registry did not read or
// its hashes are not sorted, logged once per world and tried again 10 s later.
bool types(const mem::Source &source, uint64_t world);

// A kind the walk saw on a living enemy, with what it read off it once settled (enemies.cpp): a row for a new kind,
// its empty stats filled. Stats once written stay: the first given are the ones kept.
void kind(uint64_t key, const Stats &stats);

// A faction the walk saw.
void faction(uint32_t hash);

// Takes what the game threads queued, then writes the files with changes, each at most once every 2 s unless `force`.
// The walk thread calls it on every wake, and with force once more before it ends; a file that fails that last write
// has the keys it lost logged. A game that exits without the unload loses at most the last 2 s.
void step(bool force);

}  // namespace crsrt::dict
