// snapshot.h — the probe's one instrument: on F9, or when <basename>.dump appears beside the DLL, the whole ECS as it
// stands inside one runner call, written raw to <basename>.snap-<date>-<time>.bin for tools/snap.py to read.
// Every archetype's type and offset tables and its two 32-byte table entries; the chunk bytes of those holding Health.
// Nothing is decoded here: what a byte means is decided in Python, where a wrong guess costs a rerun of the script
// instead of a rebuild and an inject.
#pragma once

namespace crsrt::snapshot {

// runner::Tick: called on the game thread inside every runner call. Takes a snapshot on the edge of F9 or on the flag.
void tick(void *world, long calls);

}  // namespace crsrt::snapshot
