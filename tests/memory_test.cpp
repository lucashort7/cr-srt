// memory_test.cpp — mem's copy and readable against memory this process owns, mapped and not.
#include "memory.h"
#include "check.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

using namespace crsrt;

TEST(copy_reads_a_buffer) {
    uint8_t src[64], out[64] = {};
    for (int i = 0; i < 64; i++) src[i] = static_cast<uint8_t>(i * 3);
    CHECK(mem::copy(src, out, sizeof out));
    CHECK(std::memcmp(src, out, sizeof out) == 0);
}

// A reserved page is in the address space and not committed: a read there faults, a copy fails.
TEST(copy_fails_on_a_reserved_page) {
    void *page = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(page != nullptr);
    if (!page) return;
    uint8_t out[16];
    CHECK(!mem::copy(page, out, sizeof out));
    CHECK(!mem::readable(page, sizeof out));
    VirtualFree(page, 0, MEM_RELEASE);
}

// The last bytes of a committed page followed by a reserved one: the copy that crosses into it fails whole.
TEST(copy_fails_across_the_end_of_a_region) {
    auto *base = static_cast<uint8_t *>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    CHECK(base != nullptr);
    if (!base) return;
    CHECK(VirtualAlloc(base, 4096, MEM_COMMIT, PAGE_READWRITE) == base);
    uint8_t out[32];
    CHECK(mem::copy(base + 4096 - 16, out, 16));
    CHECK(!mem::copy(base + 4096 - 16, out, 32));
    VirtualFree(base, 0, MEM_RELEASE);
}
