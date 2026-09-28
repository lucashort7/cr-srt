// solve.cpp
#include "solve.h"

#include "game.h"
#include "log.h"
#include "pe.h"

namespace crsrt::solve {

namespace {
unsigned long off(const void *p, HMODULE exe) {
    return static_cast<unsigned long>(static_cast<const uint8_t *>(p) - reinterpret_cast<const uint8_t *>(exe));
}
}  // namespace

// get_player is one of 14 sibling instantiations with the same prologue, 0x220 apart. What tells the player's apart
// is kPlayerTag at its lookup site, which is unique in the exe: find the site, then walk back to the last prologue.
uintptr_t get_player(HMODULE exe, uintptr_t *lookup) {
    const uint8_t *lo, *hi, *hit[4];
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
uintptr_t runner(HMODULE exe) {
    const uint8_t *str = nullptr, *tlo = nullptr, *thi = nullptr;
    const pe::Pattern name{reinterpret_cast<const uint8_t *>(game::kRunnerName), nullptr,
                           sizeof game::kRunnerName - 1};
    for (const IMAGE_SECTION_HEADER &s : pe::sections(exe)) {
        const uint8_t *lo = reinterpret_cast<const uint8_t *>(exe) + s.VirtualAddress, *hi = lo + s.Misc.VirtualSize;
        if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            if (!tlo) tlo = lo, thi = hi;
            continue;
        }
        if (!(s.Characteristics & IMAGE_SCN_MEM_READ) || str) continue;
        const uint8_t *hit;
        if (pe::scan(lo, hi, name, &hit, 1) > 0) {
            while (hit > lo && hit[-1] != 0) hit--;  // the lea targets the start of the whole string
            str = hit;
        }
    }
    if (!str || !tlo) {
        log::write("scan: runner name %s - not solved", str ? "found, no .text" : "not found");
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
        log::write("scan: %d leas to the runner name - not solved", leas);
        return 0;
    }
    const pe::Pattern store{game::kRunnerStore, game::kRunnerStoreMask, sizeof game::kRunnerStore};
    const uint8_t *hit[2];
    if (pe::scan(lea, lea + 0x400 < thi ? lea + 0x400 : thi, store, hit, 2) < 1) {
        log::write("scan: runner name at exe+%lx, lea exe+%lx, no registration store after it - not solved",
                   off(str, exe), off(lea, exe));
        return 0;
    }
    const uint8_t *fn = hit[0] + 7 + *reinterpret_cast<const int32_t *>(hit[0] + 3);
    log::write("scan: runner name exe+%lx, lea exe+%lx, store exe+%lx -> runner exe+%lx (%lu ms)", off(str, exe),
               off(lea, exe), off(hit[0], exe), off(fn, exe), GetTickCount() - t0);
    return off(fn, exe);
}

}  // namespace crsrt::solve
