// facts.cpp
#include "facts.h"

#include "game.h"
#include "memory.h"

#include <cstring>

namespace crsrt::facts {

namespace {

// Entries walked past the home slot before giving up: at 29 % load (18 919 of 65 536 on 2026-09-30) a run is a few
// entries long, and a bound keeps a torn table from spinning the caller.
constexpr uint32_t kMaxProbe = 64;

}  // namespace

bool read_double(const uint8_t *dictionary, uint64_t key, double *out) {
    const uint8_t *table = nullptr;
    uint32_t capacity = 0;
    if (!dictionary || !mem::copy(dictionary + game::kFactTable, &table, sizeof table) || !table ||
        !mem::copy(dictionary + game::kFactCapacity, &capacity, sizeof capacity) || capacity == 0 ||
        (capacity & (capacity - 1)) != 0)
        return false;
    uint8_t entry[game::kFactStride];
    for (uint32_t i = 0; i < kMaxProbe; i++) {
        const uint32_t slot = (static_cast<uint32_t>(key) + i) & (capacity - 1);
        if (!mem::copy(table + static_cast<uintptr_t>(slot) * game::kFactStride, entry, sizeof entry)) return false;
        uint16_t type = 0;
        std::memcpy(&type, entry + game::kFactType, sizeof type);
        if (type == game::kFactEmpty) return false;
        uint64_t k = 0;
        std::memcpy(&k, entry + game::kFactKey, sizeof k);
        if (k != key) continue;
        if (type != game::kFactDouble) return false;
        std::memcpy(out, entry + game::kFactValue, sizeof *out);
        return true;
    }
    return false;
}

}  // namespace crsrt::facts
