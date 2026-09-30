// game.h — everything cr-srt knows about CONTROLResonant.exe, in one file: a game patch changes this file and the
// cache, nothing else. Offsets are exe-relative; hashes are FNV-1a 32 of a component type's full name. What a
// faction or a kind is called is names.h, which grows as the game is played and not when it is patched.
#pragma once

#include <cmath>
#include <cstdint>

namespace crsrt::game {

inline constexpr wchar_t kExeName[] = L"CONTROLResonant.exe";

// --- Presenting. The game presents through one wrapper, once a frame. Its only argument is an object whose [0] is
// the engine's d3d::SwapChain: [0] HWND, [1] IDXGISwapChain*, [2] FFX context; byte +0x62 set = Streamline made
// it, +0x68 = 1 with DLSS frame generation, 2 with FSR's.
inline constexpr uintptr_t kPresentWrapper = 0x1cfead0;
inline constexpr uint8_t kPresentWrapperPrologue[] = {0x4c, 0x8b, 0xdc, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x60};
inline constexpr uintptr_t kDirectQueueGlobal = 0x5c37248;  // *(*(exe+off)+0x10) = the Direct ID3D12CommandQueue*
inline constexpr uintptr_t kDeviceGlobal = 0x5c371e8;       // *(exe+off) = the ID3D12Device*

// --- The ECS world, read the way get_player reads it. With w = the world:
//   chunk(arch)  = *(u64*)(w + (arch + 10) * 8)
//   hashes(arch) = *(u64*)(w + 8 + (arch + 0xc22) * 32 + 8), count at +0x1c of the same entry
//   offs(arch)   = *(u64*)(w + 8 + arch * 32 + 0x18450), a u32 offset from the chunk per type index
//   row          = chunk + offs[i], i = the type's index in hashes
//   tags(arch)   = *(u64*)(w + 8 + (arch + 0xc22) * 32), count at +0x18: the full type list, the columns plus the
//                  tags that carry no bytes (probe/snapshot.cpp reads it; the enemy note, 2026-09-28)
inline constexpr int64_t kArchetypeMax = 0x4000;
inline constexpr int64_t kChunkSlotBias = 10;
inline constexpr int64_t kTypeEntryBias = 0xc22;
inline constexpr uintptr_t kOffsetsBase = 0x18450;
inline constexpr uintptr_t kFindTypeIndex = 0x327F790;  // the game's own int(hashes, count, hash)
// The chunk slots end where the archetype entries begin: past this index a "slot" is an entry's pointer. The highest
// slot in use was 1267 (2026-09-28).
inline constexpr int64_t kChunkSlots = (8 + kTypeEntryBias * 32) / 8 - kChunkSlotBias;
// The world object is one readable region of 447 KB (2026-09-28), which ends before the entry of slot 0x2bbd: a walk
// bounds itself by the region, not by kChunkSlots.
// A chunk: a 16-byte header, then one 8-byte entity handle per row (u32 id, u32 generation), then the component
// columns, each its capacity in rows plus under 16 bytes of alignment padding. It holds exactly its entities.
inline constexpr uintptr_t kEntityColumn = 0x10;

// --- Damage, for the probe (hooks/damage.h). Every entity's hit goes through this one function, the player's and each
// enemy's: take_damage(row, src, cfg, ent, amount, now, evt, bus, tags), the float it returns read from xmm0 by its
// callers. row is the Health row; ent the caller's per-entity 8-byte column (ctx[4]+0x10+i*8), likely the entity's
// handle; src a 6-byte row, and a non-zero short at src+2 makes the call return 0 untouched. Decompiled in
// control-resonant-modding/decompiled/take_damage_exe+0x247EFA0.c. The 2026-09-24 build only: checked by its
// prologue (mov rax,rsp ... sub rsp,0x150), neither cached nor solved, since a probe does not earn a solve.
inline constexpr uintptr_t kTakeDamage = 0x247efa0;
inline constexpr uint8_t kTakeDamagePrologue[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89,
                                                  0x68, 0x18, 0x48, 0x89, 0x70, 0x20, 0x57, 0x41, 0x56,
                                                  0x41, 0x57, 0x48, 0x81, 0xec, 0x50, 0x01, 0x00, 0x00};
inline constexpr uint32_t kHealthRowSize = 0x38;

// --- Component type hashes.
inline constexpr uint32_t kPlayerTag = 0x59f99e92;  // the immediate inside get_player; its name is unknown
inline constexpr uint32_t kHealth = 0x3dc2bc5c;     // heron::health::component::Health: +0 fraction, +4 max points
inline constexpr uint32_t kHealthLink = 0xddc84e0c;
inline constexpr uint32_t kHealthFlags = 0x6432f1af;
inline constexpr uint32_t kDead = 0x6e63a89c;
inline constexpr uint32_t kHealthComponent = 0xa471f608;  // contentgame::HealthComponent, crheal's h_hc
inline constexpr uint32_t kRegeneration = 0x8b9bc037;
// Written by player_status::update_combat_status from the living NPCEnemy rows.
inline constexpr uint32_t kCombatStatus = 0x17560154;
// Read by 14 systems (fast travel, the quest tracker, fall_respawn) for the Gap state. Not a tag: an 8-byte column on
// the player's archetype in the Gap and out of it (1546 both, 2026-09-30). Its byte +0 is the flag, 1 in the Gap and 0
// in the world; the other 7 bytes did not move across a round trip (AKTH-0110).
inline constexpr uint32_t kInGap = 0xaa799e1b;  // heron::gap::component::InGap
inline constexpr uint32_t kInGapRowSize = 8;
inline constexpr uint64_t kInGapFlag = 0xff;  // the mask of the flag byte in the row read as a u64
// What hud_visibility::system reads off the player beside the envs, 8 bytes; meaning unread (AKTH-0110).
inline constexpr uint32_t kStatus = 0x72d38548;  // heron::player_status::component::Status
inline constexpr uint32_t kStatusRowSize = 8;
// What an NPC's kind might be read from; the damage probe dumps them (names by tools/typenames.py, 2026-09-28).
inline constexpr uint32_t kDebugName = 0x0bca075d;    // coregame::component::DebugName, 16 bytes
inline constexpr uint32_t kNpcLevel = 0x712eb585;     // heron::leveling::component::NPCLevel, 8 bytes
inline constexpr uint32_t kFactionData = 0x3796cba6;  // heron::faction::component::FactionData, 4 bytes
inline constexpr uint32_t kSpawnedNpc = 0x4967e683;   // heron::npc_spawner::component::SpawnedNPC, 0x60 bytes
// The enemy walk (enemies.h). Row sizes are the types' own: a small column's width over its capacity counts padding.
inline constexpr uint32_t kWorldTransform = 0x6cfbb2a9;  // coregame::component::WorldTransformReadOnly
inline constexpr uint32_t kAttachment = 0x5cf3ff20;      // heron::health_attachments::component::Attachment
inline constexpr uint32_t kSpawnedNpcRowSize = 0x60;
inline constexpr uint32_t kSpawnedKind = 0x30;    // 8 bytes, the enemy's kind
inline constexpr uint32_t kSpawnedByte3c = 0x3c;  // 1 byte, meaning unknown: 2 on every enemy met, 4 on two big ones
inline constexpr uint32_t kSpawnedSource = 0;    // 8 bytes shaped like an entity handle, repeated at +8: the spawner's?
inline constexpr uint32_t kFactionRowSize = 4;   // FNV-1a 32 of the faction's lowercase name
inline constexpr uint32_t kNpcLevelRowSize = 8;  // the level in the first int
inline constexpr uint32_t kWorldTransformRowSize = 0x20;
inline constexpr uint32_t kWorldPosition = 0x10;     // three floats, Y up
inline constexpr uint32_t kWorldPositionPad = 0x1c;  // a zero float after them
inline constexpr uint32_t kAttachmentRowSize = 8;    // the owner's entity handle
// The player's speed (player.h): the facing direction as a unit float3 at +0, a copy of the position at +0x10, and the
// speed at +0x20, written each frame as the length of a velocity vector whose column is not resolved yet.
inline constexpr uint32_t kMovement = 0x59fd0f7a;  // heron::movement::component::Movement
inline constexpr uint32_t kMovementRowSize = 0x30;
inline constexpr uint32_t kMovementSpeed = 0x20;  // metres a second, ~2 ticks late, 0 through most of a jump
// The player's velocity: MovementState +0, a float3 in metres a second, Y up, the vector Movement+0x20 is the length
// of. Its neighbour MovementRequest (0xee6701db) holds the displacement per tick at +0x10 and a copy at +0x20.
inline constexpr uint32_t kMovementState = 0xe6b9cb0b;  // coregame::component::MovementState
inline constexpr uint32_t kMovementStateRowSize = 0x40;

// --- What the pattern scans look for (solve.cpp), when the cache misses.
// get_player is one of 14 sibling instantiations; the player's is the one whose lookup site carries kPlayerTag.
inline constexpr uint8_t kGetPlayerLookup[] = {0x49, 0x8b, 0x5c, 0xfe, 0x48, 0x48, 0x8d, 0x8f, 0,
                                               0,    0,    0,    0x48, 0xc1, 0xe1, 0x05, 0x42, 0x8b,
                                               0x54, 0x31, 0x1c, 0x41, 0xb8, 0x92, 0x9e, 0xf9, 0x59};
inline constexpr uint8_t kGetPlayerLookupMask[] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1,
                                                   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
// mov [rsp+8],rbx; mov [rsp+18],rsi; push rbp; push rdi; push r14; lea rbp,[rsp-50]; sub rsp,150
inline constexpr uint8_t kGetPlayerPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24,
                                                 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24,
                                                 0xb0, 0x48, 0x81, 0xec, 0x50, 0x01, 0x00, 0x00};
// The runner, heron::healing::regeneration_update::system, is found by its name: the one lea that references
// this string, then the registration store `lea rax, fn; mov [reg+0x140], rax` within 0x400 bytes after it. The
// register differs per system (r15 for the runner, rsi for the sonar, r12 elsewhere); the slot does not.
inline constexpr char kRunnerName[] = "ecs::system::getName<void __cdecl heron::healing::regeneration_update::system(";
inline constexpr int32_t kSystemFnSlot = 0x140;

// --- The camera, as RadarPlus reads it (crmods/RadarPlus, its log and projectWorld): the view-projection matrix at
// +0x1c8 of the camera at exe+0x5c2c020, 16 doubles, clip = [x y z 1] * M, screen X = (x+1)/2*W, Y = (1-y)/2*H. The
// global is the camera itself: its getter is `lea rax,[camera]; ret`. The 2026-09-24 build only.
inline constexpr uintptr_t kCamera = 0x5c2c020;
inline constexpr uintptr_t kCameraViewProj = 0x1c8;

// --- A system's envs. The runner's third argument holds at +0x58 a tray of pointers, one 8-byte slot per parameter of
// the system's signature, in its order: regeneration_update's Requests, FixedTime and LuaEvents sit in slots 2-4 (its
// decompile, ghidra/out/runner.txt), crrank reads update_player_marker's DistrictDatabase and ActiveDistrict in 2-3.
inline constexpr uintptr_t kEnvTray = 0x58;

// --- The district (AKTH-0106). heron::ui_sonar::update_exploration_sonar::system reads, in slots 0-14: its entity,
// four queries, SonarData, QuestHUD, ui_map_detail::Database, ZonesMetadata, DistrictMarkerDatabase,
// DistrictDatabase, DistrictRuntime, ActiveDistrict, world_state::Database, PhysicsScene. Found by name like the
// runner.
inline constexpr char kSonarName[] =
    "ecs::system::getName<void __cdecl heron::ui_sonar::update_exploration_sonar::system(";
inline constexpr int kSonarSlots = 15;
inline constexpr uintptr_t kSonarZonesMetadata = 8 * 8;
inline constexpr uintptr_t kSonarDistrictDatabase = 10 * 8;
inline constexpr uintptr_t kSonarDistrictRuntime = 11 * 8;
inline constexpr uintptr_t kSonarActiveDistrict = 12 * 8;
// Measured 2026-09-29 in pid 1612 against the map's zone panel (The Park 3, 340/700).
// DistrictDatabase: an array of 0x230-byte entries at +0x128, their count (u32) at +0x130; each entry's id inline at
// +0x1d0 (ZONE_DOWNTOWN .. THE_GAP, 8 of them) and at +0x218 the u32 hash the other two envs key on.
inline constexpr uintptr_t kDistrictArray = 0x128;
inline constexpr uintptr_t kDistrictCount = 0x130;
inline constexpr uintptr_t kDistrictStride = 0x230;
inline constexpr uintptr_t kDistrictName = 0x1d0;
inline constexpr uintptr_t kDistrictHash = 0x218;
// DistrictRuntime: an array of 0x88-byte records at +0x8, their count (u32) at +0x10, and the XP each level costs, a
// u32 array at +0x18 with its count at +0x20 (500, 600 .. 6000, 15 of them). A record: +0x60 the district's hash,
// +0x64 its max level, +0x68 the levels completed, +0x74 its XP since level 1.
inline constexpr uintptr_t kRuntimeArray = 0x8;
inline constexpr uintptr_t kRuntimeCount = 0x10;
inline constexpr uintptr_t kRuntimeCosts = 0x18;
inline constexpr uintptr_t kRuntimeCostCount = 0x20;
inline constexpr uintptr_t kRuntimeStride = 0x88;
inline constexpr uintptr_t kRecordHash = 0x60;
inline constexpr uintptr_t kRecordMaxLevel = 0x64;
inline constexpr uintptr_t kRecordLevels = 0x68;
inline constexpr uintptr_t kRecordXp = 0x74;
// ActiveDistrict: a list at +0 of 16-byte entries, their count (u32) at +0x8; each entry's district hash at +0x8 and
// another hash at +0xc that differs between entries of one district, a candidate for the section.
inline constexpr uintptr_t kActiveCount = 0x8;
inline constexpr uintptr_t kActiveStride = 0x10;
inline constexpr uintptr_t kActiveDistrictHash = 0x8;
inline constexpr uintptr_t kActiveOtherHash = 0xc;

// A Health row in points: its fraction of max, rounded to the nearest point.
inline int points(float fraction, int32_t max) {
    return static_cast<int>(std::lround(fraction * static_cast<float>(max)));
}

}  // namespace crsrt::game
