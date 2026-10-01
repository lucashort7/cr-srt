// fixtures_test.cpp — the fixtures the tests read are where ctest's working directory says, and are what they claim.
#include "check.h"

#include <cstdio>
#include <cstring>

// cr-probe's F9 snapshot of 2026-09-28 12:53:51: 1134 archetypes, 23 chunks with Health.
TEST(the_snapshot_fixture_is_a_crsnap03) {
    FILE *f = std::fopen("fixtures/snap-20260928-125351.bin", "rb");
    CHECK(f != nullptr);
    if (!f) return;
    char magic[8] = {};
    CHECK(std::fread(magic, 1, sizeof magic, f) == sizeof magic);
    CHECK(std::memcmp(magic, "CRSNAP03", 8) == 0);
    std::fclose(f);
}
