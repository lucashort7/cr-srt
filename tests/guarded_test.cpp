// guarded_test.cpp — the guarded copy against memory this process owns, mapped and not, with its handler in place.
#include "guarded.h"
#include "check.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

using namespace crsrt;

TEST(a_guarded_copy_reads_a_buffer) {
    CHECK(guarded::install());
    uint8_t src[300], out[300] = {};
    for (int i = 0; i < 300; i++) src[i] = static_cast<uint8_t>(i * 7);
    CHECK(guarded::copy(out, src, sizeof out));
    CHECK(std::memcmp(src, out, sizeof out) == 0);
    CHECK(guarded::copy(out, src, 0));
}

TEST(a_guarded_copy_fails_on_a_reserved_page) {
    CHECK(guarded::install());
    void *page = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(page != nullptr);
    if (!page) return;
    uint8_t out[16];
    CHECK(!guarded::copy(out, page, sizeof out));
    CHECK(!guarded::copy(out, nullptr, 8));
    VirtualFree(page, 0, MEM_RELEASE);
}

// The handler sends the fault back to the copy's own exit: the next copy, and this function's frame, are intact.
TEST(a_guarded_copy_fails_across_a_regions_end_and_copies_again) {
    CHECK(guarded::install());
    auto *base = static_cast<uint8_t *>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    CHECK(base != nullptr);
    if (!base) return;
    CHECK(VirtualAlloc(base, 4096, MEM_COMMIT, PAGE_READWRITE) == base);
    std::memset(base, 0x5a, 4096);
    uint8_t out[32] = {};
    CHECK(!guarded::copy(out, base + 4096 - 16, 32));
    CHECK(guarded::copy(out, base + 4096 - 16, 16));
    CHECK(out[0] == 0x5a);
    CHECK(out[15] == 0x5a);
    VirtualFree(base, 0, MEM_RELEASE);
}

TEST(a_guarded_copy_fails_on_a_guard_page) {
    CHECK(guarded::install());
    auto *page = static_cast<uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD));
    CHECK(page != nullptr);
    if (!page) return;
    uint8_t out[8];
    CHECK(!guarded::copy(out, page, sizeof out));
    VirtualFree(page, 0, MEM_RELEASE);
}
