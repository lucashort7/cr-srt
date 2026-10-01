// solve.cpp
#include "solve.h"

#include "game.h"
#include "log.h"
#include "pe.h"

#include <cstring>

namespace crsrt::solve {

namespace {
unsigned long off(const void *p, HMODULE exe) {
    return static_cast<unsigned long>(static_cast<const uint8_t *>(p) - reinterpret_cast<const uint8_t *>(exe));
}

// Whether the string at `s` (bounded by its section's end) is a system's getName: past one, the store search has walked
// into the next system's registration and would take its function (update_loading_state took
// action_wait_one_fixed_update's, 2026-09-30).
bool names_a_system(const uint8_t *s, const uint8_t *hi) {
    static constexpr char kMark[] = "ecs::system::getName<";
    constexpr size_t kLen = sizeof kMark - 1;
    for (const uint8_t *p = s; p + kLen <= hi && p < s + 256 && *p; p++)
        if (std::memcmp(p, kMark, kLen) == 0) return true;
    return false;
}
}  // namespace

// get_player is one of 14 sibling instantiations with the same prologue, 0x220 apart. What tells the player's apart
// is kPlayerTag at its lookup site, which is unique in the exe: find the site, then walk back to the last prologue.
uintptr_t get_player(HMODULE exe, uintptr_t *lookup) {
    const uint8_t *lo = nullptr, *hi = nullptr, *hit[4];
    if (!pe::text_section(exe, &lo, &hi)) {
        log::write("scan: no executable section");
        return 0;
    }
    const pe::Pattern site{game::kGetPlayerLookup, game::kGetPlayerLookupMask, sizeof game::kGetPlayerLookup};
    DWORD t0 = GetTickCount();
    int n = pe::scan(lo, hi, site, hit, 4);
    DWORD ms = GetTickCount() - t0;
    if (n != 1) {
        log::write("scan: getp_lookup %d hits in %lu ms - not solved", n, ms);
        return 0;
    }
    *lookup = off(hit[0], exe);
    const pe::Pattern prologue{game::kGetPlayerPrologue, nullptr, sizeof game::kGetPlayerPrologue};
    const uint8_t *start = hit[0] - 0x400 < lo ? lo : hit[0] - 0x400;
    const uint8_t *found = nullptr;
    for (const uint8_t *p = start; p < hit[0]; p++)
        if (pe::match_at(p, prologue)) found = p;
    if (!found) {
        log::write("scan: getp_lookup exe+%lx but no prologue in the 0x400 before - not solved",
                   static_cast<unsigned long>(*lookup));
        return 0;
    }
    log::write("scan: getp_lookup exe+%lx (1 hit, %lu ms over %lu KB), prologue exe+%lx",
               static_cast<unsigned long>(*lookup), ms, static_cast<unsigned long>((hi - lo) / 1024), off(found, exe));
    return off(found, exe);
}

// 80 systems register the same way, and the three that share the runner's prologue differ only in RIP-relative
// globals, so no byte pattern picks it. Its name does: 1 string, 1 lea to it, 1 registration store after the lea.
uintptr_t runner(HMODULE exe) { return system(exe, game::kRunnerName, "runner"); }

uintptr_t system(HMODULE exe, const char *name_text, const char *what) {
    const uint8_t *str = nullptr, *tlo = nullptr, *thi = nullptr, *slo = nullptr, *shi = nullptr;
    const pe::Pattern name{reinterpret_cast<const uint8_t *>(name_text), nullptr, lstrlenA(name_text)};
    for (const IMAGE_SECTION_HEADER &s : pe::sections(exe)) {
        const uint8_t *lo = reinterpret_cast<const uint8_t *>(exe) + s.VirtualAddress, *hi = lo + s.Misc.VirtualSize;
        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            if (!tlo) tlo = lo, thi = hi;
            continue;
        }
        if (!(s.Characteristics & IMAGE_SCN_MEM_READ) || str) continue;
        const uint8_t *hit = nullptr;
        if (pe::scan(lo, hi, name, &hit, 1) > 0) {
            while (hit > lo && hit[-1] != 0) hit--;  // the lea targets the start of the whole string
            str = hit;
            slo = lo, shi = hi;
        }
    }
    if (!str || !tlo) {
        log::write("scan: %s name %s - not solved", what, str ? "found, no .text" : "not found");
        return 0;
    }
    DWORD t0 = GetTickCount();
    const uint8_t *lea = nullptr;
    int leas = 0;
    for (const uint8_t *p = tlo; p + 7 <= thi; p++) {  // lea r64, [rip+rel32], rax..rdi
        if (p[0] != 0x48 || p[1] != 0x8d || (p[2] & 0xc7) != 0x05) continue;
        if (p + 7 + *reinterpret_cast<const int32_t *>(p + 3) == str) {
            if (!lea) lea = p;
            leas++;
        }
    }
    if (leas != 1) {
        log::write("scan: %d leas to the %s name - not solved", leas, what);
        return 0;
    }
    // lea rax, [rip+fn] then mov [reg+disp32], rax: REX.W (+B), 89, modrm mod=10 reg=rax, a SIB byte when rm=100
    const uint8_t *hit[1] = {nullptr};
    for (const uint8_t *p = lea + 7; p + 16 <= thi && p < lea + 0x400 && !hit[0]; p++) {
        if ((p[0] == 0x48 || p[0] == 0x4c) && p[1] == 0x8d && (p[2] & 0xc7) == 0x05) {  // lea r64, [rip+rel32]
            const uint8_t *target = p + 7 + *reinterpret_cast<const int32_t *>(p + 3);
            if (target >= slo && target < shi && target != str && names_a_system(target, shi)) {
                log::write(
                    "scan: %s name at exe+%lx, lea exe+%lx, the next system's name at exe+%lx before any "
                    "registration store - not solved",
                    what, off(str, exe), off(lea, exe), off(p, exe));
                return 0;
            }
        }
        if (p[0] != 0x48 || p[1] != 0x8d || p[2] != 0x05) continue;
        const uint8_t *q = p + 7;
        if ((q[0] != 0x48 && q[0] != 0x49) || q[1] != 0x89 || (q[2] & 0xf8) != 0x80) continue;
        const uint8_t *disp = q + 3 + ((q[2] & 7) == 4 ? 1 : 0);
        if (*reinterpret_cast<const int32_t *>(disp) == game::kSystemFnSlot) hit[0] = p;
    }
    if (!hit[0]) {
        log::write("scan: %s name at exe+%lx, lea exe+%lx, no registration store after it - not solved", what,
                   off(str, exe), off(lea, exe));
        return 0;
    }
    const uint8_t *fn = hit[0] + 7 + *reinterpret_cast<const int32_t *>(hit[0] + 3);
    log::write("scan: %s name exe+%lx, lea exe+%lx, store exe+%lx -> %s exe+%lx (%lu ms)", what, off(str, exe),
               off(lea, exe), off(hit[0], exe), what, off(fn, exe), GetTickCount() - t0);
    return off(fn, exe);
}

uintptr_t script_function(HMODULE exe, const char *name_text) {
    const uint8_t *str = nullptr, *tlo = nullptr, *thi = nullptr;
    // the name with its terminator, and a terminator before it: the whole string, not the tail of a longer one
    const pe::Pattern name{reinterpret_cast<const uint8_t *>(name_text), nullptr, lstrlenA(name_text) + 1};
    for (const IMAGE_SECTION_HEADER &s : pe::sections(exe)) {
        const uint8_t *lo = reinterpret_cast<const uint8_t *>(exe) + s.VirtualAddress, *hi = lo + s.Misc.VirtualSize;
        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            if (!tlo) tlo = lo, thi = hi;
            continue;
        }
        if (!(s.Characteristics & IMAGE_SCN_MEM_READ) || str) continue;
        const uint8_t *hits[8];
        const int n = pe::scan(lo, hi, name, hits, 8);
        for (int i = 0; i < n && !str; i++)
            if (hits[i] > lo && hits[i][-1] == 0) str = hits[i];
    }
    if (!str || !tlo) {
        log::write("scan: script %s %s - not solved", name_text, str ? "found, no .text" : "not found");
        return 0;
    }
    const uint8_t *lea = nullptr;
    int leas = 0;
    for (const uint8_t *p = tlo; p + 7 <= thi; p++) {
        if (p[0] != 0x48 || p[1] != 0x8d || (p[2] & 0xc7) != 0x05) continue;
        if (p + 7 + *reinterpret_cast<const int32_t *>(p + 3) == str) {
            if (!lea) lea = p;
            leas++;
        }
    }
    if (leas != 1 || lea + 28 > thi) {
        log::write("scan: %d leas to script %s - not solved", leas, name_text);
        return 0;
    }
    // 48 89 85 d32 (mov [rbp+d], rax), 48 8d 05 rel32 (lea rax, [fn]), 48 89 85 d32+8
    const uint8_t *q = lea + 7;
    const bool shape = q[0] == 0x48 && q[1] == 0x89 && q[2] == 0x85 && q[7] == 0x48 && q[8] == 0x8d && q[9] == 0x05 &&
                       q[14] == 0x48 && q[15] == 0x89 && q[16] == 0x85 &&
                       *reinterpret_cast<const int32_t *>(q + 17) == *reinterpret_cast<const int32_t *>(q + 3) + 8;
    if (!shape) {
        log::write("scan: script %s lea exe+%lx is not a {name, function} pair - not solved", name_text, off(lea, exe));
        return 0;
    }
    const uint8_t *fn = q + 14 + *reinterpret_cast<const int32_t *>(q + 10);
    if (fn < tlo || fn >= thi) {
        log::write("scan: script %s points outside .text - not solved", name_text);
        return 0;
    }
    log::write("scan: script %s lea exe+%lx -> exe+%lx", name_text, off(lea, exe), off(fn, exe));
    return off(fn, exe);
}

}  // namespace crsrt::solve
