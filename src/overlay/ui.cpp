// ui.cpp
#include "overlay/ui.h"

#include "enemies.h"
#include "game.h"
#include "hooks/runner.h"
#include "hooks/sonar.h"
#include "hooks/transit.h"
#include "hooks/uistate.h"
#include "log.h"
#include "memory.h"
#include "names.h"
#include "player.h"
#include "sequence.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

#include "imgui.h"

namespace crsrt::ui {

namespace {

// Draws with no runner call in between before the health reads as stale: ~0.5 s at 60 fps.
constexpr long kStaleDraws = 30;
constexpr long kIniEvery = 60;  // draws between two reads of [enemies] in the ini, ~1 s
// Seconds with no runner call before the zone's clock stops: the game is paused. A pause costs the clock at most this.
constexpr float kClockGrace = 0.1f;
// Seconds a new zone must hold before the clock takes it. Inside one district the game's active list empties for a few
// looks (one every ~0.5 s): the log of 2026-09-29 has 16 gaps with the same district on both sides, logged to the
// whole second as 1, 1, 2, 4, 6 s and up, so the shortest three last at most 3 s.
constexpr double kZoneSettle = 3.5;

// The panel as Lucas drew it over a 1920x1080 screenshot (story AKTH-0109). Every size is in pixels at a height of 1080
// and scales with the text, which has one size: the SRT's 11 px at 1080, following the height, never under 11.
constexpr float kPanelPx = 11.0f;
constexpr float kAnchorX = 21.0f / 1920.0f, kAnchorY = 123.0f / 1080.0f;  // the box's top left, of the screen
constexpr float kBoxW = 208.0f;  // the least width: a wider line widens the box
constexpr float kPadX = 12.0f, kPadY = 6.0f;
constexpr float kLine = 14.0f;                                             // one line of text
constexpr float kDividerGap = 2.0f, kHeaderGap = 2.0f, kBoxBottom = 2.0f;  // under a section, the header, the last row
constexpr float kBarW = 97.0f, kBarH = 9.0f, kShieldH = 3.0f, kShieldGap = 1.0f, kNumberGap = 6.0f, kRowGap = 6.0f;
constexpr ImU32 kBoxFill = IM_COL32(0, 0, 0, 128);       // black at 50 %
constexpr ImU32 kBoxLine = IM_COL32(255, 255, 255, 64);  // white at 25 %: the border and the two dividers
constexpr ImU32 kText = IM_COL32(255, 255, 255, 235);
constexpr ImU32 kHealthLeft = IM_COL32(0x37, 0xCB, 0x3D, 230);     // the SRT's green
constexpr ImU32 kHealthMissing = IM_COL32(0x34, 0x31, 0xBF, 230);  // the SRT's blue, under every health bar
constexpr ImU32 kAllyLeft = IM_COL32(28, 255, 255, 230);           // the cyan the game draws over its own allies
constexpr ImU32 kShieldLeft = IM_COL32(240, 240, 240, 230);
constexpr ImU32 kShieldTrack = IM_COL32(0, 0, 0, 150);

long g_seen_calls, g_still;  // the runner's count at the last draw, and draws since it last moved
float g_quiet;               // seconds since it last moved, up to 1
bool g_stale_said;

// [enemies] radius=30 rows=8, [allies] rows=4: the panel shows the enemies within `radius` metres, at most `rows` of
// them, and the allies within the same radius under them, at most `[allies] rows`, only while there is one.
int g_radius = 30, g_rows = 8, g_ally_rows = 4;
long g_ini_at = -kIniEvery;
// [areas] <id's first 8 hex digits>=<label>: the name of a level, as Lucas writes it; g_area_label is the label read
// for g_area_labelled. Every id is shown, labelled or not, so an unmapped one is seen and named (2026-10-01).
uint64_t g_area_labelled;
char g_area_label[32];

enemies::List g_list;  // the render thread's copy, one per draw

// The zone's clock. g_zone is the zone it runs in, the sonar's id or "" outside every district, and g_shown that zone
// as the sonar last read it, for the zone line. g_away is the running time since the sonar last read g_zone, which no
// zone owns until one is taken. g_next is a district the sonar reads that has not held kZoneSettle yet, g_next_held how
// long it has, and g_empty the running time of the current stretch with no district.
char g_zone[32], g_next[32];
double g_zone_secs, g_away, g_next_held, g_empty;
sonar::District g_shown, g_next_shown;

// White on a drop shadow, for what the overlay draws outside the panel.
void text_at(ImVec2 at, float px, const char *text, ImU32 color = IM_COL32(255, 255, 255, 235)) {
    ImFont *font = ImGui::GetFont();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->AddText(font, px, ImVec2(at.x + 2.0f, at.y + 2.0f), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(font, px, at, color, text);
}

// Bottom centre, clear of the crosshair: the overlay is alive.
void injected_line(float w, float h, float px) {
    static const char text[] = "dll injected: yes";
    ImVec2 size = ImGui::GetFont()->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    text_at(ImVec2(std::floor((w - size.x) * 0.5f), std::floor(h - size.y - h * 0.03f)), px, text);
}

// True when the runner has not been called for kStaleDraws draws: everything it published is held over.
bool runner_stale(long calls, long frame, float dt) {
    if (calls != g_seen_calls) {
        g_seen_calls = calls;
        g_still = 0;
        g_quiet = 0.0f;
    } else {
        if (g_still < kStaleDraws) g_still++;
        if (g_quiet < 1.0f) g_quiet += dt;
    }
    bool stale = g_still >= kStaleDraws;
    if (stale != g_stale_said) {
        log::write("overlay: health %s at runner call %ld, overlay frame %ld",
                   stale ? "stale, the runner stopped" : "fresh again", calls, frame);
        g_stale_said = stale;
    }
    return stale;
}

// A value that rounds to zero at one decimal, as 0: "-0.0" reads as a direction that is not there.
float tenth(float v) { return std::fabs(v) < 0.05f ? 0.0f : v; }

void read_ini(long frame) {
    if (frame - g_ini_at < kIniEvery) return;
    g_ini_at = frame;
    static const std::wstring ini = log::path(L".ini");
    g_radius = static_cast<int>(GetPrivateProfileIntW(L"enemies", L"radius", 30, ini.c_str()));
    g_rows = static_cast<int>(GetPrivateProfileIntW(L"enemies", L"rows", 8, ini.c_str()));
    g_rows = std::min(g_rows, enemies::kMaxRows);
    g_ally_rows = static_cast<int>(GetPrivateProfileIntW(L"allies", L"rows", 4, ini.c_str()));
    g_ally_rows = std::min(g_ally_rows, enemies::kMaxRows);
    const uint64_t area = transit::area();
    if (area != g_area_labelled) {
        wchar_t key[16], label[32];
        std::swprintf(key, std::size(key), L"%08lx", static_cast<unsigned long>(area >> 32));
        GetPrivateProfileStringW(L"areas", key, L"", label, static_cast<DWORD>(std::size(label)), ini.c_str());
        std::snprintf(g_area_label, sizeof g_area_label, "%ls", label);
        g_area_labelled = area;
    }
}

// "hiss", or the hash's first four digits.
void faction_label(uint32_t hash, char *out, size_t cap) {
    for (const auto &f : names::kFactions)
        if (f.hash == hash) {
            std::snprintf(out, cap, "%s", f.name);
            return;
        }
    std::snprintf(out, cap, "%04x", static_cast<unsigned>(hash >> 16));
}

// "artillery", "moldbag_melee?" for a guess, or the kind's first four hex digits (all sixteen when `whole`).
void kind_label(const uint8_t kind[8], bool whole, char *out, size_t cap) {
    uint64_t k = 0;
    std::memcpy(&k, kind, sizeof k);
    for (const auto &n : names::kKinds)
        if (n.hash == k) {
            std::snprintf(out, cap, "%s%s", n.name, n.source == names::Source::Paired ? "?" : "");
            return;
        }
    if (whole)
        std::snprintf(out, cap, "%02x%02x%02x%02x%02x%02x%02x%02x", kind[0], kind[1], kind[2], kind[3], kind[4],
                      kind[5], kind[6], kind[7]);
    else
        std::snprintf(out, cap, "%02x%02x", kind[0], kind[1]);
}

// "central_trial", or the machine's hash when the table lacks it.
const char *machine_label(uint32_t hash) {
    for (const auto &m : names::kMachines)
        if (m.hash == hash) return m.name;
    static char hex[12];
    std::snprintf(hex, sizeof hex, "%08x", static_cast<unsigned>(hash));
    return hex;
}

bool shown(const enemies::Enemy &e) { return e.distance < 0.0f || e.distance <= static_cast<float>(g_radius); }

// The size of the labels at the enemies' feet.
float number_px(float px) { return std::floor(px * 0.65f); }

// The panel's one text size.
float panel_px(float height) { return std::max(kPanelPx, std::floor(height / 1080.0f * kPanelPx)); }

// Where the panel's next line goes. `right` is the furthest x any line reached, so the box is sized after its text.
struct Pen {
    ImDrawList *dl = nullptr;
    float px = 0.0f, s = 1.0f;  // the text size, and the scale from 1080
    float x = 0.0f, y = 0.0f;   // the left of the text, the top of the next line
    float right = 0.0f;
    float dividers[3] = {};  // zone | player | enemies | allies
    int ndividers = 0;
};

// A size at 1080, at this height.
float scaled(const Pen &pen, float v) { return std::floor(v * pen.s); }

// Text at x, y, flat: the box behind it is what keeps it readable.
void text(Pen &pen, float x, float y, const char *s) {
    pen.dl->AddText(ImGui::GetFont(), pen.px, ImVec2(x, y), kText, s);
    pen.right = std::max(pen.right, x + ImGui::GetFont()->CalcTextSizeA(pen.px, FLT_MAX, 0.0f, s).x);
}

void line(Pen &pen, const char *s) {
    text(pen, pen.x, pen.y, s);
    pen.y += scaled(pen, kLine);
}

// The end of a section: a divider under its last line, drawn with the box once the box's width is known.
void divide(Pen &pen) {
    const float y = pen.y + scaled(pen, kDividerGap);
    if (pen.ndividers < 3) pen.dividers[pen.ndividers++] = y;
    pen.y = y + scaled(pen, kPadY);
}

// A bar `fraction` full on its track.
void bar(ImDrawList *dl, ImVec2 at, float width, float height, float fraction, ImU32 fill, ImU32 track) {
    if (!(fraction >= 0.0f)) fraction = 0.0f;
    fraction = std::min(fraction, 1.0f);
    dl->AddRectFilled(at, ImVec2(at.x + width, at.y + height), track);
    if (fraction > 0.0f) dl->AddRectFilled(at, ImVec2(at.x + std::floor(width * fraction), at.y + height), fill);
}

// The clock starts over in `key` with every second since it last read the old zone, or, when `key` is that zone, keeps
// them: the time away was a flicker.
void take(const char *key, const sonar::District &shown) {
    if (std::strncmp(key, g_zone, sizeof g_zone) == 0) {
        g_zone_secs += g_away;
    } else {
        std::snprintf(g_zone, sizeof g_zone, "%s", key);
        g_zone_secs = g_away;
    }
    g_shown = shown;
    g_next[0] = '\0';
    g_away = g_next_held = g_empty = 0.0;
}

// Seconds in the zone the sonar last resolved: from zero at every change of zone, a return included, and still while
// the game is paused. Outside every district is a zone of its own. A district is taken once it has held kZoneSettle, a
// stretch with no district once it has lasted that long, and a shorter stretch counts for the district around it.
void zone_clock(const sonar::District &d, float dt) {
    const double run = g_quiet <= kClockGrace ? dt : 0.0;
    const char *key = d.has ? d.id : "";
    const bool gap = key[0] == '\0' && g_next[0] != '\0';  // no district while one is held pending
    if (!gap && std::strncmp(key, g_zone, sizeof g_zone) == 0) {
        g_zone_secs += g_away + run;
        g_shown = d;
        g_next[0] = '\0';
        g_away = g_next_held = g_empty = 0.0;
        return;
    }
    g_away += run;
    if (key[0] == '\0') {
        g_empty += run;
        g_next_held += run;
        if (g_empty >= kZoneSettle) take("", d);
        return;
    }
    g_empty = 0.0;
    if (std::strncmp(key, g_next, sizeof g_next) != 0) {
        std::snprintf(g_next, sizeof g_next, "%s", key);
        g_next_held = 0.0;
    }
    g_next_held += run;
    g_next_shown = d;
    if (g_next_held >= kZoneSettle) take(key, g_next_shown);
}

// The zone the clock runs in, its clock and where the player stands: the zone line and the clock change together. The
// zone is held while the game is paused, where it cannot change, so the map can be read against it.
void zone_section(Pen &pen, const player::State &p, bool stale, float dt) {
    sonar::District d;
    sonar::district(d);
    zone_clock(d, dt);
    char s[96];
    // The sonar names no district in the Gap or in a challenge; the player's InGap flag and ChallengeModeInfo do.
    float elapsed = 0.0f;
    uint32_t machine = 0;
    if (!g_shown.has && !stale && p.in_gap)
        std::snprintf(s, sizeof s, "zone: THE_GAP");
    else if (!g_shown.has && uistate::challenge(&elapsed, &machine))
        std::snprintf(s, sizeof s, "zone: CHALLENGE %s", machine_label(machine));
    else if (!g_shown.has)
        std::snprintf(s, sizeof s, "zone: --");
    else if (g_shown.level == 0)
        std::snprintf(s, sizeof s, "zone: %s", g_shown.id);
    else
        std::snprintf(s, sizeof s, "zone: %s [lv%d]", g_shown.id, g_shown.level);
    if (!stale && p.in_story) std::strncat(s, " [story]", sizeof s - std::strlen(s) - 1);
    line(pen, s);
    // the level the player is in: the Luau call that entered it, else GameState's level id by its [areas] label or its
    // first digits, a zone, a building or an arena alike
    char seq[64];
    const uint64_t area = transit::area();
    if (sequence::current(seq, sizeof seq))
        std::snprintf(s, sizeof s, "section: %s", seq);
    else if (area && g_area_label[0] && area == g_area_labelled)
        std::snprintf(s, sizeof s, "section: %s", g_area_label);
    else if (area)
        std::snprintf(s, sizeof s, "section: %08lx", static_cast<unsigned long>(area >> 32));
    else
        std::snprintf(s, sizeof s, "section: --");
    line(pen, s);
    const long secs = static_cast<long>(g_zone_secs + g_away);
    std::snprintf(s, sizeof s, "ts: %02ld:%02ld%s", secs / 60, secs % 60, uistate::paused() ? " [paused]" : "");
    line(pen, s);
    if (stale || !p.has_position)
        std::snprintf(s, sizeof s, "loc: --");
    else
        std::snprintf(s, sizeof s, "loc: (%.1f, %.1f, %.1f)", tenth(p.position[0]), tenth(p.position[1]),
                      tenth(p.position[2]));
    line(pen, s);
}

// The player: Health and Power, the game's Main Stats, each with its upgrade levels at the end, then Build Power and
// Aberrant Strength, Source and Talent Points, and the movement. "--" for a value not read yet, or not read now: a
// held-over value would read as live.
void player_section(Pen &pen, const player::State &p, bool stale) {
    char s[96];
    // Written as a negation so a NaN fraction is refused too; the DeMorgan form would let it through.
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (stale || !p.has_health || !(p.fraction >= 0.0f && p.fraction < 100.0f) || p.max <= 0) {
        std::snprintf(s, sizeof s, "hp: -- / -- [--%%] (--/--)");
    } else {
        const int hp = game::points(p.fraction, p.max);
        std::snprintf(s, sizeof s, "hp: %d / %d [%d%%] (--/--)%s", hp, static_cast<int>(p.max),
                      static_cast<int>(static_cast<long long>(hp) * 100 / p.max), p.dead ? " [dead]" : "");
    }
    line(pen, s);
    line(pen, "pw: -- / -- [--%] (--/--)");
    line(pen, "bp: --   as: --");
    double source = 0.0, talent = 0.0;
    if (stale || !uistate::currency(&source, &talent))
        std::snprintf(s, sizeof s, "src: --   tp: --");
    else
        std::snprintf(s, sizeof s, "src: %d   tp: %d", static_cast<int>(source), static_cast<int>(talent));
    line(pen, s);
    if (stale || !p.has_speed)
        std::snprintf(s, sizeof s, "ms: --");
    else
        std::snprintf(s, sizeof s, "ms: %.1f m/s", p.speed);
    line(pen, s);
    if (stale || !p.has_velocity)
        std::snprintf(s, sizeof s, "ground: --   air: --");
    else
        std::snprintf(s, sizeof s, "ground: %.1f   air: %.1f", tenth(p.h), tenth(p.velocity[1]));
    line(pen, s);
}

// One enemy: what the walk read (faction, kind, level, distance) over its health bar, the shield a thin white bar above
// it when one stands, and the points and the percent beside it.
void enemy_row(Pen &pen, const enemies::Enemy &e) {
    char faction[16], kind[40], s[96];
    faction_label(e.faction, faction, sizeof faction);
    kind_label(e.kind, false, kind, sizeof kind);
    if (e.distance < 0.0f)
        std::snprintf(s, sizeof s, "%s %s lv%d", faction, kind, static_cast<int>(e.level));
    else
        std::snprintf(s, sizeof s, "%s %s lv%d %.0fm", faction, kind, static_cast<int>(e.level), e.distance);
    line(pen, s);

    const float bar_w = scaled(pen, kBarW), bar_h = scaled(pen, kBarH),
                shield_h = std::max(1.0f, scaled(pen, kShieldH));
    const bool shielded = e.shield_max > 0 && e.shield_fraction > 0.0f;
    if (shielded) bar(pen.dl, ImVec2(pen.x, pen.y), bar_w, shield_h, e.shield_fraction, kShieldLeft, kShieldTrack);
    const float bar_y = pen.y + shield_h + scaled(pen, kShieldGap);
    bar(pen.dl, ImVec2(pen.x, bar_y), bar_w, bar_h, e.fraction, names::is_ally(e.faction) ? kAllyLeft : kHealthLeft,
        kHealthMissing);
    const int hp = game::points(e.fraction, e.max);
    // Tenths of a percent, cut rather than rounded: a scratched enemy reads 99.9 %, never 100.0 %.
    const long long tenths = e.max > 0 ? static_cast<long long>(hp) * 1000 / e.max : 0;
    std::snprintf(s, sizeof s, "%d/%d %.1f%%", hp, static_cast<int>(e.max), static_cast<double>(tenths) / 10.0);
    text(pen, pen.x + bar_w + scaled(pen, kNumberGap), std::floor(bar_y + (bar_h - pen.px) * 0.5f), s);
    pen.y = bar_y + bar_h + scaled(pen, kRowGap);
}

// One side of the list: the count within the radius of the count loaded, the SRT's "12 of 16", then per row within
// it, nearest first, at most `rows` of them. `ally` picks the side, by the faction hash the walk read on that pass.
void side(Pen &pen, bool ally, const char *name, int total, int rows) {
    int within = 0;
    for (int i = 0; i < g_list.count; i++)
        if (shown(g_list.rows[i]) && names::is_ally(g_list.rows[i].faction) == ally) within++;
    char s[48];
    std::snprintf(s, sizeof s, "%s: %d of %d", name, within, total);
    line(pen, s);
    pen.y += scaled(pen, kHeaderGap);
    int drawn = 0;
    for (int i = 0; i < g_list.count && drawn < rows; i++) {
        if (!shown(g_list.rows[i]) || names::is_ally(g_list.rows[i].faction) != ally) continue;
        enemy_row(pen, g_list.rows[i]);
        drawn++;
    }
}

// The enemies, then the allies under them only while the walk holds one: a commanded crowd would otherwise fill the
// enemies' rows and their count, and most of the game has no ally at all.
void enemy_section(Pen &pen, bool stale, long frame) {
    read_ini(frame);
    if (stale) {
        line(pen, "enemies: --");
        return;
    }
    enemies::read(g_list);
    side(pen, false, "enemies", g_list.total - g_list.allies, g_rows);
    if (g_list.allies == 0) return;
    divide(pen);
    side(pen, true, "allies", g_list.allies, g_ally_rows);
}

// The panel: the zone, the player and the enemies in one box at the drawing's place. The text goes on the draw list's
// second channel as it is laid out, and the box on the first once its size is known, so the box sits behind the text.
void panel(float w, float h, const player::State &p, bool stale, long frame, float dt) {
    Pen pen;
    pen.dl = ImGui::GetForegroundDrawList();
    pen.px = panel_px(h);
    pen.s = pen.px / kPanelPx;
    const ImVec2 box(std::floor(w * kAnchorX), std::floor(h * kAnchorY));
    pen.x = box.x + scaled(pen, kPadX);
    pen.y = box.y + scaled(pen, kPadY);
    pen.right = pen.x;

    pen.dl->ChannelsSplit(2);
    pen.dl->ChannelsSetCurrent(1);
    zone_section(pen, p, stale, dt);
    divide(pen);
    player_section(pen, p, stale);
    divide(pen);
    enemy_section(pen, stale, frame);

    pen.dl->ChannelsSetCurrent(0);
    const ImVec2 end(std::max(box.x + scaled(pen, kBoxW), pen.right + scaled(pen, kPadX)),
                     pen.y + scaled(pen, kBoxBottom));
    pen.dl->AddRectFilled(box, end, kBoxFill);
    pen.dl->AddRect(box, end, kBoxLine);
    // One pixel row between the border's two columns: a divider that reached them would light the joins twice.
    for (int i = 0; i < pen.ndividers; i++)
        pen.dl->AddRectFilled(ImVec2(box.x + 1.0f, pen.dividers[i]), ImVec2(end.x - 1.0f, pen.dividers[i] + 1.0f),
                              kBoxLine);
    pen.dl->ChannelsMerge();
}

// The camera's view-projection matrix, or false. It is 16 doubles, read twice until both reads agree (a torn copy is
// refused, RadarPlus's way), and refused when not finite, too large, or with a zero w column. The first one kept is
// logged with the player's position through it, the eyes' check.
bool view_proj(float m[16], const player::State &p) {
    static const uint8_t *const matrix =
        reinterpret_cast<const uint8_t *>(GetModuleHandleW(game::kExeName)) + game::kCamera + game::kCameraViewProj;
    static bool logged;
    double a[16], b[16];
    int tries = 0;
    do {
        if (!mem::copy(matrix, a, sizeof a) || !mem::copy(matrix, b, sizeof b)) return false;
        // Byte equality is the test: two reads that differ in any byte are a torn copy, whatever the doubles say.
        // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison)
    } while (std::memcmp(a, b, sizeof a) != 0 && ++tries < 4);
    if (tries == 4) return false;
    double w = 0.0;
    for (int i = 0; i < 16; i++) {
        if (!std::isfinite(a[i]) || std::fabs(a[i]) > 1e6) return false;
        m[i] = static_cast<float>(a[i]);
    }
    for (int i = 3; i < 16; i += 4) w += std::fabs(a[i]);
    if (!(w > 0.0)) return false;
    if (!logged && p.has_position) {
        logged = true;
        const float x = p.position[0], y = p.position[1] + 1.0f, z = p.position[2];
        const float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
        const float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        const float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
        char row[160];  // log::write is wsprintf, which has no floats
        std::snprintf(row, sizeof row, "player at clip %g %g w %g -> screen %.3f %.3f", cx, cy, cw,
                      cw != 0.0f ? cx / cw : 0.0f, cw != 0.0f ? cy / cw : 0.0f);
        log::write("labels: matrix at %p kept: %s", matrix, row);
    }
    return true;
}

// Under each enemy the panel shows, its faction and kind at its feet: the walk's position through the camera.
void labels(float w, float h, float px, const player::State &p, bool stale) {
    float m[16];
    if (stale || !view_proj(m, p)) return;
    const float lpx = number_px(px);
    char text[72];
    for (int i = 0; i < g_list.count; i++) {
        const enemies::Enemy &e = g_list.rows[i];
        if (!shown(e)) continue;
        const float x = e.position[0], y = e.position[1], z = e.position[2];
        const float cx = x * m[0] + y * m[4] + z * m[8] + m[12];
        const float cy = x * m[1] + y * m[5] + z * m[9] + m[13];
        const float cw = x * m[3] + y * m[7] + z * m[11] + m[15];
        if (!(cw > 1e-4f)) continue;  // behind the camera
        const float nx = cx / cw, ny = cy / cw;
        if (std::fabs(nx) > 1.0f || std::fabs(ny) > 1.0f) continue;
        char faction[16], kind[40];
        faction_label(e.faction, faction, sizeof faction);
        kind_label(e.kind, true, kind, sizeof kind);
        std::snprintf(text, sizeof text, "%s %s", faction, kind);
        const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(lpx, FLT_MAX, 0.0f, text);
        text_at(
            ImVec2(std::floor((nx + 1.0f) * 0.5f * w - size.x * 0.5f), std::floor((1.0f - ny) * 0.5f * h + lpx * 0.3f)),
            lpx, text);
    }
}

}  // namespace

float font_px(float height) {
    float px = std::floor(height / 1440.0f * 26.0f);
    return px < 16.0f ? 16.0f : px;
}

void draw(float width, float height, float px, long frame) {
    // Every glyph the overlay can show, measured each frame at every size it draws. Glyphs bake the first time they are
    // drawn, and one first seen mid-game re-uploads the atlas with an unbounded wait on the game's queue; measured here
    // they bake with the atlas's first upload, and afterwards this is a cache lookup.
    static const char glyphs[] =
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
        "abcdefghijklmnopqrstuvwxyz{|}~\xC2\xB7";
    for (float size : {px, number_px(px), panel_px(height)})
        ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, glyphs);
    injected_line(width, height, px);

    player::State p;
    long calls = 0;
    runner::player_state(p, &calls);
    const float dt = ImGui::GetIO().DeltaTime;
    const bool stale = runner_stale(calls, frame, dt);
    panel(width, height, p, stale, frame, dt);
    labels(width, height, px, p, stale);
}

}  // namespace crsrt::ui
