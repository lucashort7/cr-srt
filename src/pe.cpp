// pe.cpp
#include "pe.h"

namespace crsrt::pe {

std::span<const IMAGE_SECTION_HEADER> sections(HMODULE module) {
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    const auto *nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64 *>(reinterpret_cast<const uint8_t *>(module) + dos->e_lfanew);
    return {IMAGE_FIRST_SECTION(nt), nt->FileHeader.NumberOfSections};
}

bool text_section(HMODULE module, const uint8_t **lo, const uint8_t **hi) {
    // A search that writes the section's bounds, not a test: any_of would need a lambda with side effects.
    // NOLINTNEXTLINE(readability-use-anyofallof)
    for (const IMAGE_SECTION_HEADER &s : sections(module)) {
        if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        *lo = reinterpret_cast<const uint8_t *>(module) + s.VirtualAddress;
        *hi = *lo + s.Misc.VirtualSize;
        return true;
    }
    return false;
}

bool match_at(const uint8_t *p, const Pattern &pattern) {
    for (int i = 0; i < pattern.len; i++)
        if ((!pattern.mask || pattern.mask[i]) && p[i] != pattern.bytes[i]) return false;
    return true;
}

int scan(const uint8_t *lo, const uint8_t *hi, const Pattern &pattern, const uint8_t **out, int max) {
    int n = 0;
    const uint8_t first = pattern.bytes[0];  // every pattern here starts on a fixed byte
    for (const uint8_t *p = lo; p + pattern.len <= hi; p++) {
        if (*p != first || !match_at(p, pattern)) continue;
        if (n < max) out[n] = p;
        n++;
    }
    return n;
}

}  // namespace crsrt::pe
