// script.cpp
#include "hooks/script.h"

#include "ecs.h"
#include "game.h"
#include "lifecycle.h"
#include "log.h"
#include "memory.h"
#include "solve.h"

#include "MinHook.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace crsrt::script {

namespace {

using ScriptFn = int (*)(void *);

constexpr LONG kLogged = 64;

// Luau's lua_State: top at +0x08, base at +0x10, userdata at +0x78 (a pointer to the world's holder). This build's
// TValue is 24 bytes, the type at +0x10, as the exe's lua_type reads it (exe+2c4d8e0: index * 24 from base, type at
// +0x10). Luau's types: 0 nil, 1 boolean, 2 light userdata, 3 number, 4 vector, 5 string, 6 table, 7 function, 8
// userdata. A TString: length at +0x14, the bytes at +0x18 (Luau's layout, unverified in this build). An entity is a
// light userdata whose value is its handle, id in the low half (nl_game_jump_to reads it through exe+4684b0).
constexpr uintptr_t kTop = 0x08, kBase = 0x10, kUserdata = 0x78;
constexpr uintptr_t kValueSize = 24, kValueType = 0x10, kValueExtra = 0x08;
constexpr int kMaxArgs = 6;
constexpr uintptr_t kStringLen = 0x14, kStringData = 0x18;
constexpr uint32_t kStringShown = 256;  // a jump point path ran to 119 (2026-09-30)
// world + 0x58530: one u64 per entity id, the archetype in its low 16 bits (0xffff for none), the row in its high
// half (nl_game_jump_to, exe+1a1aa83). The game's own entity check (exe+4684b0) first wants tag 1, the id under the
// count at +0x58510 and the generation in the low half of +0x584e8's u64 for that id equal to the handle's high half.
constexpr uintptr_t kEntityRecords = 0x58530, kEntityCount = 0x58510, kEntityGenerations = 0x584e8;
constexpr int32_t kEntityTag = 1;
constexpr uint32_t kNoArchetype = 0xffff;

// The (archetype, type list) pairs the log has, so each is written once: the game hands an index to another type set
// within one process (arch 760 in pid 8084, 2026-09-30), so the index alone is not the key.
SRWLOCK g_listed_lock = SRWLOCK_INIT;
constexpr int kListedMax = 64;
uint64_t g_listed[kListedMax];
int g_listed_count;
bool g_listed_full_said;

bool first_listing(uint64_t key) {
    AcquireSRWLockExclusive(&g_listed_lock);
    bool seen = false;
    for (int i = 0; i < g_listed_count && !seen; i++) seen = g_listed[i] == key;
    const bool fresh = !seen && g_listed_count < kListedMax;
    if (fresh) g_listed[g_listed_count++] = key;
    const bool full_now = !seen && !fresh && !g_listed_full_said;
    if (full_now) g_listed_full_said = true;
    ReleaseSRWLockExclusive(&g_listed_lock);
    if (full_now) log::write("script: %d type lists logged, no more", kListedMax);
    return fresh;
}

// " arch A row R", or "" when the record is unreadable, and the archetype's type list once.
void entity_place(const uint8_t *L, uint64_t handle, char *out, size_t cap) {
    out[0] = 0;
    const uint32_t id = static_cast<uint32_t>(handle & 0xffffffffULL);
    const uint8_t *holder = nullptr, *world = nullptr, *records = nullptr, *generations = nullptr;
    uint64_t count = 0, generation = 0, record = 0;
    if (!mem::copy(L + kUserdata, &holder, sizeof holder) || !holder || !mem::copy(holder, &world, sizeof world) ||
        !world || !mem::copy(world + kEntityCount, &count, sizeof count) ||
        !mem::copy(world + kEntityGenerations, &generations, sizeof generations) || !generations ||
        !mem::copy(world + kEntityRecords, &records, sizeof records) || !records)
        return;
    if (id == 0xffffffffu || id >= count ||
        !mem::copy(generations + static_cast<uintptr_t>(id) * 8, &generation, sizeof generation) ||
        static_cast<uint32_t>(generation) != static_cast<uint32_t>(handle >> 32)) {
        std::snprintf(out, cap, " not alive");
        return;
    }
    if (!mem::copy(records + static_cast<uintptr_t>(id) * 8, &record, sizeof record)) return;
    const uint32_t arch = static_cast<uint32_t>(record & 0xffff);
    if (arch == kNoArchetype || arch >= static_cast<uint32_t>(game::kArchetypeMax)) {
        std::snprintf(out, cap, " no archetype");
        return;
    }
    std::snprintf(out, cap, " arch %lu row %lu", static_cast<unsigned long>(arch),
                  static_cast<unsigned long>(record >> 32));
    ecs::TypeList list;
    if (!ecs::type_list(world, arch, list)) return;
    uint32_t sum = 2166136261u;  // FNV-1a over the list: the key's other half
    for (uint32_t i = 0; i < list.count; i++) sum = (sum ^ list.hashes[i]) * 16777619u;
    if (first_listing((static_cast<uint64_t>(arch) << 32) | sum)) ecs::log_type_list(arch, list);
}

volatile LONG g_inflight;

struct Function {
    const char *name;
    ScriptFn original = nullptr;
    LONG logged = kLogged;  // calls logged in full, then one per kEvery
    volatile LONG calls = 0;
};
constexpr LONG kEvery = 100;

Function g_functions[] = {
    {"return_point_save"},
    {"return_point_save_then_switch_to_level"},
    {"clear_return_point_and_save"},
    {"clear_return_point_then_switch_to_level"},
    {"save_then_switch_to_level"},
    {"player_transition"},
    {"zone_transition"},
    {"reload_game"},
    {"nl_game_jump_to"},
    {"nl_game_last_jump_point"},
    {.name = "nl_teleport", .logged = 8},  // a script calls it hundreds of times a second (2026-09-30 21:26)
    {"enter_gap"},
    {"exit_gap"},
    {"set_active_challenge"},
    {"activate_challenge_mode"},
};
constexpr int kFunctions = static_cast<int>(std::size(g_functions));

// One argument as text: its type and value, a string's bytes when printable.
void arg_text(const uint8_t *L, const uint8_t *slot, char *out, size_t cap) {
    uint8_t tv[kValueSize];
    if (!mem::copy(slot, tv, sizeof tv)) {
        std::snprintf(out, cap, "unreadable");
        return;
    }
    uint64_t value = 0;
    int32_t extra = 0, type = 0;
    std::memcpy(&value, tv, 8);
    std::memcpy(&extra, tv + kValueExtra, 4);
    std::memcpy(&type, tv + kValueType, 4);
    switch (type) {
        case 0: std::snprintf(out, cap, "nil"); return;
        case 1: std::snprintf(out, cap, "bool %s", value & 1 ? "true" : "false"); return;
        case 2: {
            char place[64];
            if (extra == kEntityTag)
                entity_place(L, value, place, sizeof place);
            else
                place[0] = 0;
            std::snprintf(out, cap, "light userdata tag %ld %08lx%08lx%s", static_cast<long>(extra),
                          static_cast<unsigned long>(value >> 32), static_cast<unsigned long>(value & 0xffffffffULL),
                          place);
            return;
        }
        case 3: {
            double d = 0;
            std::memcpy(&d, &value, 8);
            std::snprintf(out, cap, "number %.9g", d);
            return;
        }
        case 5: {
            const auto *ts = reinterpret_cast<const uint8_t *>(value);
            uint32_t len = 0;
            char text[kStringShown + 1] = {};
            if (!mem::copy(ts + kStringLen, &len, sizeof len)) break;
            const uint32_t shown = len < kStringShown ? len : kStringShown;
            if (!mem::copy(ts + kStringData, text, shown)) break;
            for (uint32_t i = 0; i < shown; i++)
                if (text[i] < 0x20 || text[i] > 0x7e) text[i] = '.';
            std::snprintf(out, cap, "string \"%s\"%s (len %lu)", text, len > shown ? "..." : "",
                          static_cast<unsigned long>(len));
            return;
        }
        default: break;
    }
    std::snprintf(out, cap, "type %ld extra %ld value %08lx%08lx", static_cast<long>(type), static_cast<long>(extra),
                  static_cast<unsigned long>(value >> 32), static_cast<unsigned long>(value & 0xffffffffULL));
}

void log_call(const Function &f, void *state, LONG n) {
    const auto *L = static_cast<const uint8_t *>(state);
    const uint8_t *top = nullptr, *base = nullptr;
    if (!mem::copy(L + kTop, &top, sizeof top) || !mem::copy(L + kBase, &base, sizeof base) || top < base) {
        log::write("script: %s call %ld tid %lu, L %p: stack unreadable", f.name, static_cast<long>(n),
                   GetCurrentThreadId(), state);
        return;
    }
    const long nargs = static_cast<long>((top - base) / static_cast<ptrdiff_t>(kValueSize));
    log::write("script: %s call %ld tid %lu, %ld args", f.name, static_cast<long>(n), GetCurrentThreadId(), nargs);
    // one line per argument: a path can run to hundreds of characters and a log line stops at 1024
    for (long i = 0; i < nargs && i < kMaxArgs; i++) {
        char arg[kStringShown + 96];
        arg_text(L, base + i * kValueSize, arg, sizeof arg);
        log::write("script:   %s arg %ld: %s", f.name, i + 1, arg);
    }
}

template <int I>
int hk(void *state) {
    InterlockedIncrement(&g_inflight);
    Function &f = g_functions[I];
    const LONG n = InterlockedIncrement(&f.calls);
    if (!lifecycle::stopping && (n <= f.logged || n % (f.logged < kLogged ? 10000 : kEvery) == 0))
        log_call(f, state, n);
    // A Luau error is a C++ throw (luaD_throw, exe+2c43a10) that would unwind past a decrement after the call and leave
    // the unloader waiting forever: the count drops first and the original runs as a tail call, no frame of ours left.
    const ScriptFn original = f.original;
    InterlockedDecrement(&g_inflight);
    [[gnu::musttail]] return original(state);
}

template <int... I>
constexpr auto detours(std::integer_sequence<int, I...>) {
    return std::array<ScriptFn, sizeof...(I)>{&hk<I>...};
}
constexpr auto kDetours = detours(std::make_integer_sequence<int, kFunctions>{});

}  // namespace

int install(HMODULE exe) {
    int hooked = 0;
    for (int i = 0; i < kFunctions && !lifecycle::stopping; i++) {
        Function &f = g_functions[i];
        const uintptr_t offset = solve::script_function(exe, f.name);
        if (!offset) continue;
        void *target = reinterpret_cast<uint8_t *>(exe) + offset;
        MH_STATUS ms =
            MH_CreateHook(target, reinterpret_cast<void *>(kDetours[i]), reinterpret_cast<void **>(&f.original));
        if (ms != MH_OK) {
            log::write("MH_CreateHook script %s exe+%lx: %d", f.name, static_cast<unsigned long>(offset),
                       static_cast<int>(ms));
            continue;
        }
        ms = MH_EnableHook(target);
        log::write("hook on script %s exe+%lx: %s", f.name, static_cast<unsigned long>(offset),
                   ms == MH_OK ? "enabled" : "FAILED");
        if (ms == MH_OK) hooked++;
    }
    log::write("script: %d of %d functions hooked%s", hooked, kFunctions,
               lifecycle::stopping ? " (stopped by the unloader)" : "");
    return hooked;
}

long inflight() { return g_inflight; }

}  // namespace crsrt::script
