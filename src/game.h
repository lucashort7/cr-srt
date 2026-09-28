// game.h — everything cr-srt knows about CONTROLResonant.exe, in one file: a game patch changes this file and the
// cache, nothing else. Offsets are exe-relative; hashes are FNV-1a 32 of a component type's full name.
#pragma once

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
inline constexpr int64_t kArchetypeMax = 0x4000;
inline constexpr int64_t kChunkSlotBias = 10;
inline constexpr int64_t kTypeEntryBias = 0xc22;
inline constexpr uintptr_t kOffsetsBase = 0x18450;
inline constexpr uintptr_t kFindTypeIndex = 0x327F790;  // the game's own int(hashes, count, hash)

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
inline constexpr uint32_t kCombatStatus = 0x17560154;
// What an NPC's kind might be read from; the damage probe dumps them (names by tools/typenames.py, 2026-09-28).
inline constexpr uint32_t kDebugName = 0x0bca075d;   // coregame::component::DebugName, 16 bytes
inline constexpr uint32_t kNpcLevel = 0x712eb585;    // heron::leveling::component::NPCLevel, 8 bytes
inline constexpr uint32_t kFactionData = 0x3796cba6; // heron::faction::component::FactionData, 4 bytes
inline constexpr uint32_t kSpawnedNpc = 0x4967e683;  // heron::npc_spawner::component::SpawnedNPC, 0x60 bytes

// --- What the pattern scans look for (solve.cpp), when the cache misses.
// get_player is one of 14 sibling instantiations; the player's is the one whose lookup site carries kPlayerTag.
inline constexpr uint8_t kGetPlayerLookup[] = {0x49, 0x8b, 0x5c, 0xfe, 0x48, 0x48, 0x8d, 0x8f, 0, 0, 0, 0, 0x48,
                                               0xc1, 0xe1, 0x05, 0x42, 0x8b, 0x54, 0x31, 0x1c, 0x41, 0xb8, 0x92,
                                               0x9e, 0xf9, 0x59};
inline constexpr uint8_t kGetPlayerLookupMask[] = {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1,
                                                   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
// mov [rsp+8],rbx; mov [rsp+18],rsi; push rbp; push rdi; push r14; lea rbp,[rsp-50]; sub rsp,150
inline constexpr uint8_t kGetPlayerPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24,
                                                 0x18, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24,
                                                 0xb0, 0x48, 0x81, 0xec, 0x50, 0x01, 0x00, 0x00};
// The runner, heron::healing::regeneration_update::system, is found by its name: the one lea that references
// this string, then the registration store `lea rax, fn; mov [r15+0x140], rax` within 0x400 bytes after it.
inline constexpr char kRunnerName[] =
    "ecs::system::getName<void __cdecl heron::healing::regeneration_update::system(";
inline constexpr uint8_t kRunnerStore[] = {0x48, 0x8d, 0x05, 0, 0, 0, 0, 0x49, 0x89, 0x87, 0x40, 0x01, 0x00, 0x00};
inline constexpr uint8_t kRunnerStoreMask[] = {1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1};

}  // namespace crsrt::game
