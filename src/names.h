// names.h — what a faction and a kind are called on the panel and over the enemy: the two tables that grow as the
// game is played. SRTPluginRE9 keeps the same thing in CharacterMap.h, `cp_B800` to `The Girl`, written by hand.
#pragma once

#include <cstdint>

namespace crsrt::names {

// --- Factions. FactionData's names, found by hashing candidates (2026-09-28): the hash is FNV-1a 32 of the lowercase
// name. An enemy of a faction missing here shows its hash.
struct Faction {
    uint32_t hash;
    const char *name;
};
inline constexpr uint32_t kFactionAlly = 0x538682a2;
inline constexpr uint32_t kFactionAnomaly = 0xeae53489;
inline constexpr uint32_t kFactionFbc = 0xdf2ac3e8;
inline constexpr uint32_t kFactionCommanded = 0x668dce8b;
inline constexpr Faction kFactions[] = {
    {0x4aac31a8, "hiss"},
    {0x1d82b4ed, "mold"},
    {0x2c99c300, "player"},
    {0x8c4b1ad8, "neutral"},
    {0x20408545, "explosives"},
    {0x175869f5, "hedron"},
    // What a blob spawns (2026-09-30): hostile, its own faction, no candidate hashed to it.
    {0x9417cf1c, "spawn"},
    // The player's summons, named by us: no candidate hashed to these (2026-09-29). The first is every companion,
    // the Mold ally and the Astral Spike alike, and the kind tells which; the Volatile Anomaly has its own.
    {kFactionAlly, "ally"},
    {kFactionAnomaly, "ally"},
    // The FBC Ranger and the quest escorts, a story ally (2026-09-29).
    {kFactionFbc, "fbc"},
    // An enemy taken by the Command ability (2026-09-30): the game rewrites its faction in place, kind and health
    // kept, and what it spawns from then on is born with this one.
    {kFactionCommanded, "cmd"},
};
// On the player's side now, by the hash alone: the game flips it in place, so a row can enter and leave this set.
constexpr bool is_ally(uint32_t faction) {
    return faction == kFactionAlly || faction == kFactionAnomaly || faction == kFactionFbc ||
           faction == kFactionCommanded;
}

// --- Kinds. The kind at SpawnedNPC+0x30 is eight bytes, read little-endian. Where a name comes from says how far
// to trust it.
enum class Source : uint8_t {
    // The kind is FNV-1a 64 of this name, which the encounter database in the heap holds
    // (control-resonant-modding/tools/kindnames.py). Certain, and free when the kind never shows up.
    Hashed,
    // The kind matched no name; the database pairs it with the name beside it, which fits what it is. Drawn with `?`.
    Paired,
    // Named by Lucas looking at it, in the game's own words where it has them. About 440 million candidate names
    // hashed to none of these (2026-09-29).
    Seen,
};
constexpr uint64_t fnv1a64(const char *s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++) h = (h ^ static_cast<uint8_t>(*s)) * 0x100000001b3ull;
    return h;
}
struct Kind {
    uint64_t hash;
    const char *name;
    Source source;
};
#define CRSRT_KIND(n) {fnv1a64(n), n, Source::Hashed}
inline constexpr Kind kKinds[] = {
    CRSRT_KIND("artillery"),
    CRSRT_KIND("blob"),
    CRSRT_KIND("brute"),
    CRSRT_KIND("enraged"),
    CRSRT_KIND("glider"),
    CRSRT_KIND("healer"),
    CRSRT_KIND("hermit"),
    CRSRT_KIND("master"),
    CRSRT_KIND("plague"),
    CRSRT_KIND("pursuer"),
    CRSRT_KIND("ranger"),
    CRSRT_KIND("sniper"),
    CRSRT_KIND("soldier"),
    CRSRT_KIND("spitter"),
    CRSRT_KIND("spore"),
    CRSRT_KIND("leviathan"),
    CRSRT_KIND("juggernaut"),  // Mold, 4713 hp at level 7 (2026-09-30)
    // In the database, hash unmet so far.
    CRSRT_KIND("brute_overshield"),
    CRSRT_KIND("brute_arcs_friendly"),
    CRSRT_KIND("pursuer_friendly"),
    CRSRT_KIND("healer_overshield"),
    CRSRT_KIND("glider_overshield"),
    CRSRT_KIND("miniboss_blob"),
    CRSRT_KIND("miniboss_glider_overshield"),
    CRSRT_KIND("miniboss_master"),
    CRSRT_KIND("flux_horizontal_saw_gap"),
    // The ResonanceX Spawner's catalogue (crmods/spawner.dll, 0xVo1d), its template names as the DLL stores them;
    // a name reaches the panel only on an exact hash, so a word that is no kind costs nothing (2026-10-02).
    CRSRT_KIND("ability_mold_turret"),
    CRSRT_KIND("aggregate_beams_group"),
    CRSRT_KIND("aggregate_beams_group_ngp"),
    CRSRT_KIND("aggregate_beams_solo"),
    CRSRT_KIND("aggregate_beams_solo_gap"),
    CRSRT_KIND("aggregate_group"),
    CRSRT_KIND("aggregate_group_ngp"),
    CRSRT_KIND("aggregate_solo"),
    CRSRT_KIND("aggregate_waves_group"),
    CRSRT_KIND("artillery_fbc"),
    CRSRT_KIND("astral_construct"),
    CRSRT_KIND("boss_astral_summon"),
    CRSRT_KIND("city_bus_a_li"),
    CRSRT_KIND("distorted"),
    CRSRT_KIND("dummy"),
    CRSRT_KIND("elevated"),
    CRSRT_KIND("flux"),
    CRSRT_KIND("flux_gap"),
    CRSRT_KIND("flux_oldest_house"),
    CRSRT_KIND("gap_dummy"),
    CRSRT_KIND("heavy"),
    CRSRT_KIND("house"),
    CRSRT_KIND("mini_blob_b"),
    CRSRT_KIND("miniboss_brute"),
    CRSRT_KIND("miniboss_glider"),
    CRSRT_KIND("miniboss_juggernaut"),
    CRSRT_KIND("miniboss_master_maria"),
    CRSRT_KIND("miniboss_pursuer"),
    CRSRT_KIND("miniboss_sniper"),
    CRSRT_KIND("miniboss_sniper_overshield"),
    CRSRT_KIND("mold_alarm"),
    CRSRT_KIND("named"),
    CRSRT_KIND("other"),
    CRSRT_KIND("plague_balloons"),
    CRSRT_KIND("prop"),
    CRSRT_KIND("pursuer_overshield"),
    CRSRT_KIND("ranger_escort_a"),
    CRSRT_KIND("ranger_escort_b"),
    CRSRT_KIND("ranger_flashback"),
    CRSRT_KIND("ranger_seized"),
    CRSRT_KIND("seekers_ability"),
    CRSRT_KIND("soldier_fbc"),
    CRSRT_KIND("swarm_group"),
    CRSRT_KIND("swarm_group_ngp"),
    CRSRT_KIND("swarm_melee_group"),
    CRSRT_KIND("swarm_melee_group_ngp"),
    CRSRT_KIND("swarm_melee_solo"),
    CRSRT_KIND("swarm_solo"),
    CRSRT_KIND("vending_machine"),
    {0xc9126a04f8f5aa92ull, "miniboss_leviathan", Source::Paired},
    {0x0ffb453d3c75872aull, "moldbag_melee", Source::Paired},
    {0xc2d353a9fa6fce18ull, "moldbag_shotgun", Source::Paired},
    {0x0ae36649c3edfaa6ull, "moldbag_rocketlauncher", Source::Paired},
    {0x0c8622242419f585ull, "brute_arcs", Source::Paired},
    {0xac2379f122058f56ull, "glider_overshield", Source::Paired},  // 1980 + 1208 at level 6 (2026-10-02)
    {0x504daed11f390c7cull, "sniper_overshield", Source::Paired},
    {0x22a6ec1be4b02ed0ull, "drifter_charge", Source::Paired},
    {0xb5c87403f8aa4595ull, "flux_horizontal_saw", Source::Paired},
    {0xf54130012b8252e2ull, "miniboss_pursuer_overshield", Source::Paired},  // "WILHEM", 22756 hp lv9, Pattern waves
    // 737 hp at level 9, its own faction 9417cf1c shared with 35a0c348427de187
    {0xb5001270c3072968ull, "mini_blob", Source::Paired},
    {0xee00b1418052d936ull, "juggernaut_geysers", Source::Paired},  // Mold, 800 hp beside the juggernaut
    {0x1a46c02ada381f6aull, "miniboss_plague", Source::Paired},     // "FERGUS", Mold, 50273 hp at level 7
    {0x22798b9f13fb6a55ull, "spore_mines", Source::Paired},         // Mold, 3579 hp beside it
    // The player's companions, by the names the game's menus give them.
    {0xdc8de3fce9d33935ull, "mold_ally", Source::Seen},
    {0xa418d99ab84ff184ull, "astral_spike", Source::Seen},
    {0x74cea16c9c24e0baull, "volatile_anomaly", Source::Seen},
    // A named elite (the HUD gives it a boss_name_generic_* name, "JUHA" 2026-09-30): no record in the encounter
    // database in two scans, no candidate hashes to it. a sniper by its look, Lucas's word for the tier; its kind sat
    // beside boss_name_generic_sniper_* in memory. "ADAM" (2026-09-30) was a plain leviathan by kind: a boss name does
    // not imply a kind of its own.
    {0xba50a0bb3a1fa5d7ull, "sniper_elite", Source::Seen},
    {0x2279fa96f365c023ull, "hedron_factor",
     Source::Seen},  // "HEDRON FACTOR", the first sequence's boss, 39052 hp lvl 8
    {0x2279fa36f365c023ull, "hedron_spawn", Source::Seen},  // seen with it, 5997 hp at level 1
    {0x58474eec591376a0ull, "specimen", Source::Seen},  // "SPECIMEN A" and "B", a Mold quest boss, 20947 hp at level 7
};
#undef CRSRT_KIND
static_assert(fnv1a64("artillery") == 0x75689b4f8f103711ull, "the artillery kind read in the walk");

// --- Arcade machines, by ChallengeModeInfo+0x10: no candidate of the 10 765 names in the encounter database hashes
// to one (2026-09-30), so each is named by its zone and what its cabinet runs, as Lucas saw it.
struct Machine {
    uint32_t hash;
    const char *name;
};
inline constexpr Machine kMachines[] = {
    {0xe8187e3d, "central_trial"},    // SHUM III, a checkpoint time trial with traps, 3 minutes at most
    {0x74641c62, "pattern_waves"},    // SHUM III, survive the encounter waves, scored
    {0xab586530, "downtown_defend"},  // SHUM III, defend the valuables and defeat all enemies, scored
    {0xb6bd6e10, "twisted_jrpg"},     // the JRPG minigame, scored (build 1.4.0, 2026-10-01)
};

}  // namespace crsrt::names
