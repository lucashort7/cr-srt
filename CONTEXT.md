# cr-srt, the words

The names the code and its notes use for the game's things and for its own parts. A module named after a concept here
uses the word as written.

## The game

- **Zone**: the district the sonar resolves around the player (`ZONE_HISS`), with its level; outside every district
  is a zone of its own, drawn `--`. The Gap and a challenge name the zone when the sonar names none.
- **Level**: the id at `GameState+0x90`, a zone, a building, an arena or a sequence; 64 bits, drawn by its first 8 hex
  digits when nothing names it.
- **Section**: what the zone section's second line names: a fast travel's area, the level's name, its `[areas]` label,
  or its digits.
- **Clock** (`ts`): seconds in the zone, from zero at every change of zone, still while the game is paused.
- **Fast travel point**: the `areas/<name>` segment of the asset path a TransitionRequest carries.
- **Runner**: the ECS system cr-srt reads the player from, once a tick, `heron::healing::regeneration_update::system`.
- **Walk**: the enemy list, read by a thread of cr-srt's off the runner's last world.

## cr-srt's parts

- **Whereabouts** (`whereabouts.h`): the zone, the section and the clock, from what the hooks read; the overlay asks it
  for one View a frame.
- **Source** (`source.h`): where game memory is copied from: the live process, or a CRSNAP03 snapshot in the tests.
- **Guarded copy** (`guarded.h`): a copy whose access violation returns false, at memcpy's cost; what the live source
  reads with.
- **World**, **Columns** (`ecs.h`): the slot table and its entries, and one archetype's columns, copied.
- **Dictionary** (`dict.h`): every type, kind and faction met, in `dict\*.tsv` beside the DLL; a key is written once,
  when it is new, and the walk thread is the only writer.
- **Hook**, **Call** (`hooks/registry.h`): one hooked function, and one call inside its detour; the registry holds every
  hook for the unloader and the meters.
