// whereabouts_test.cpp — the zone section replayed frame by frame: the clock against the rules in whereabouts.h and the
// death of 2026-09-29 23:27 in the log, the naming of a level, the [areas] file.
#include "whereabouts.h"
#include "check.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

using namespace crsrt;

namespace {

constexpr uint64_t kHiss = 0x01c080962a7d636dULL;   // ZONE_HISS's level, 2026-10-01
constexpr uint64_t kTower = 0x184dcbbd71e5463dULL;  // a tower inside it

sonar::District district(const char *id, int level = 0) {
    sonar::District d;
    d.has = id[0] != '\0';
    std::snprintf(d.id, sizeof d.id, "%s", id);
    d.level = level;
    return d;
}

// `seconds` of frames 0.5 s apart in `zone` ("" for none), the runner running unless `quiet`.
whereabouts::View play(const char *zone, double seconds, whereabouts::Inputs in = {}, bool quiet = false) {
    in.district = district(zone, 3);
    in.run = quiet ? 0.0 : 0.5;
    whereabouts::View v{};
    const long frames = std::lround(seconds * 2.0);
    for (long i = 0; i < frames; i++) whereabouts::frame(in, v);
    return v;
}

bool is(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

}  // namespace

TEST(a_zone_is_taken_after_it_holds_and_counts_from_there) {
    whereabouts::reset();
    whereabouts::View v = play("ZONE_HISS", 3.0);
    CHECK(is(v.zone, "--"));  // not held long enough
    v = play("ZONE_HISS", 7.0);
    CHECK(is(v.zone, "ZONE_HISS [lv3]"));
    CHECK(v.seconds == 10);
}

TEST(the_clock_stands_while_the_runner_is_quiet) {
    whereabouts::reset();
    play("ZONE_HISS", 10.0);
    const whereabouts::View v = play("ZONE_HISS", 60.0, {}, true);
    CHECK(v.seconds == 10);
}

TEST(a_short_stretch_without_a_district_counts_for_the_district) {
    whereabouts::reset();
    play("ZONE_HISS", 10.0);
    whereabouts::View v = play("", 3.0);
    CHECK(is(v.zone, "ZONE_HISS [lv3]"));
    CHECK(v.seconds == 13);
    v = play("ZONE_HISS", 2.0);
    CHECK(v.seconds == 15);
}

TEST(a_new_district_starts_from_the_time_it_took_to_hold) {
    whereabouts::reset();
    play("ZONE_HISS", 10.0);
    const whereabouts::View v = play("ZONE_MOLD", 5.0);
    CHECK(is(v.zone, "ZONE_MOLD [lv3]"));
    CHECK(v.seconds == 5);
}

// 2026-09-29 23:27: the sonar lost the district 5 s before the death screen stopped the runner, the empty zone was
// taken, and the respawn into ZONE_FRONTLINE 77 s later counted as a fresh entry: `ts` read 00:08 eight seconds in.
TEST(the_death_of_2026_09_29_starts_the_clock_over) {
    whereabouts::reset();
    play("ZONE_FRONTLINE", 120.0);
    whereabouts::View v = play("", 5.0);
    CHECK(is(v.zone, "--"));
    play("", 77.0, {}, true);
    v = play("ZONE_FRONTLINE", 8.0);
    CHECK(is(v.zone, "ZONE_FRONTLINE [lv3]"));
    CHECK(v.seconds == 8);
}

TEST(the_zone_line_names_the_gap_a_challenge_and_the_story) {
    whereabouts::reset();
    whereabouts::Inputs in;
    in.in_gap = true;
    CHECK(is(play("", 1.0, in).zone, "THE_GAP"));
    in = {};
    in.challenge = true;
    in.machine = 0x12345678;
    CHECK(is(play("", 1.0, in).zone, "CHALLENGE 12345678"));
    in = {};
    in.in_story = true;
    CHECK(is(play("ZONE_HISS", 5.0, in).zone, "ZONE_HISS [lv3] [story]"));
}

TEST(a_level_takes_the_name_of_the_call_that_entered_it) {
    whereabouts::reset();
    whereabouts::level_named("levels/dungeon_jrpg", kHiss, 1000);
    whereabouts::Inputs in;
    in.level = kHiss;
    in.now_ms = 1500;
    CHECK(is(play("", 0.5, in).section, "01c08096"));  // the id has not moved yet
    in.level = kTower;
    in.now_ms = 2000;
    CHECK(is(play("", 0.5, in).section, "dungeon_jrpg"));
    in.level = kHiss;
    CHECK(is(play("", 0.5, in).section, "01c08096"));
    in.level = kTower;
    CHECK(is(play("", 0.5, in).section, "dungeon_jrpg"));  // bound for good
}

TEST(a_name_whose_level_never_moves_is_forgotten) {
    whereabouts::reset();
    whereabouts::level_named("dungeon_hedron", kHiss, 0);
    whereabouts::Inputs in;
    in.level = kTower;
    in.now_ms = whereabouts::kPendingMs + 1;
    CHECK(is(play("", 0.5, in).section, "184dcbbd"));
}

TEST(a_fast_travel_names_the_area_while_its_level_holds) {
    whereabouts::reset();
    whereabouts::Inputs in;
    in.level = kTower;
    in.point = "central_office";
    in.point_level = kTower;
    CHECK(is(play("", 0.5, in).section, "central_office"));
    in.level = kHiss;
    CHECK(is(play("", 0.5, in).section, "01c08096"));
    in = {};
    CHECK(is(play("", 0.5, in).section, "--"));
}

TEST(flush_writes_a_bound_name_and_reads_a_label) {
    whereabouts::reset();
    const wchar_t *ini = L".\\whereabouts-test.ini";
    DeleteFileW(ini);
    WritePrivateProfileStringW(L"areas", L"184dcbbd", L"hiss_tower", ini);
    whereabouts::Inputs in;
    in.level = kTower;
    play("", 0.5, in);
    whereabouts::flush(ini);
    CHECK(is(play("", 0.5, in).section, "hiss_tower"));  // a hand label
    whereabouts::level_named("taxi_puzzle", kTower, 0);
    in.level = kHiss;
    play("", 0.5, in);
    whereabouts::flush(ini);
    wchar_t got[32] = L"";
    GetPrivateProfileStringW(L"areas", L"01c08096", L"", got, 32, ini);
    CHECK(std::wcscmp(got, L"taxi_puzzle") == 0);
    DeleteFileW(ini);
}
