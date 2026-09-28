// pe.h — reading a loaded module as its PE headers describe it: its sections, and byte patterns inside them.
#pragma once

#include <windows.h>

#include <cstdint>
#include <span>

namespace crsrt::pe {

// A byte pattern; mask[i] == 1 means bytes[i] must match, 0 is a wildcard.
struct Pattern {
    const uint8_t *bytes;
    const uint8_t *mask;  // nullptr = every byte must match
    int len;
};

// The module's section table, straight from its loaded headers.
std::span<const IMAGE_SECTION_HEADER> sections(HMODULE module);

// The first executable section, as [lo, hi).
bool text_section(HMODULE module, const uint8_t **lo, const uint8_t **hi);

bool match_at(const uint8_t *p, const Pattern &pattern);

// Every hit of the pattern in [lo, hi): returns how many, and the first `max` of them in `out`.
int scan(const uint8_t *lo, const uint8_t *hi, const Pattern &pattern, const uint8_t **out, int max);

}  // namespace crsrt::pe
