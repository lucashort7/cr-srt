// ui.cpp
#include "overlay/ui.h"

#include "enemies.h"
#include "game.h"
#include "hooks/runner.h"
#include "hooks/sonar.h"
#include "log.h"
#include "memory.h"
#include "player.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"

namespace crsrt::ui {

namespace {

// Draws with no runner call in between before the health reads as stale: ~0.5 s at 60 fps.
constexpr long kStaleDraws = 30;
constexpr long kIniEvery = 60;  // draws between two reads of [enemies] in the ini, ~1 s

long g_seen_calls, g_still;  // the runner's count at the last draw, and draws since it last moved
bool g_stale_said;

// [enemies] radius=30 rows=8: the panel shows the enemies within `radius` metres, at most `rows` of them.
int g_radius = 30, g_rows = 8;
long g_ini_at = -kIniEvery;

enemies::List g_list;  // the render thread's copy, one per draw

// White on a drop shadow.
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
bool runner_stale(long calls, long frame) {
    if (calls != g_seen_calls) {
        g_seen_calls = calls;
        g_still = 0;
    } else if (g_still < kStaleDraws) {
        g_still++;
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

// The stats block on the left, one row per number: health, position, the game's speed, its velocity vector. "--" when
// the runner cannot resolve the row or has stopped being called: a held-over value would read as live. Returns the y
// below the block.
float stats(ImVec2 at, float px, const player::State &p, bool stale) {
    const float step = std::floor(px * 1.25f);
    char text[96];
    // Written as a negation so a NaN fraction is refused too; the DeMorgan form would let it through.
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (stale || !p.has_health || !(p.fraction >= 0.0f && p.fraction < 100.0f) || p.max <= 0)
        std::snprintf(text, sizeof text, "health  --");
    else
        std::snprintf(text, sizeof text, "health  %d / %d  (%.1f%%)", static_cast<int>(p.fraction * p.max + 0.5f),
                      static_cast<int>(p.max), p.fraction * 100.0f);
    text_at(at, px, text);
    if (stale || !p.has_position)
        std::snprintf(text, sizeof text, "pos  --");
    else
        std::snprintf(text, sizeof text, "pos  x %.1f  y %.1f  z %.1f", tenth(p.position[0]), tenth(p.position[1]),
                      tenth(p.position[2]));
    text_at(ImVec2(at.x, at.y + step), px, text);
    if (stale || !p.has_speed)
        std::snprintf(text, sizeof text, "speed  --");
    else
        std::snprintf(text, sizeof text, "speed  %.1f m/s", p.speed);
    text_at(ImVec2(at.x, at.y + 2.0f * step), px, text);
    if (stale || !p.has_velocity)
        std::snprintf(text, sizeof text, "vel  --");
    else
        std::snprintf(text, sizeof text, "vel  h %.1f  v %.1f  total %.1f", tenth(p.h), tenth(p.velocity[1]),
                      tenth(p.total));
    text_at(ImVec2(at.x, at.y + 3.0f * step), px, text);
    return at.y + 3.0f * step + std::floor(px * 1.8f);
}

// Under the stats block: the district the sonar last resolved, as the map names its level and bar. It is held while the
// game is paused, where it cannot change, so the map can be read against it. Returns the y below it.
float zone(ImVec2 at, float px) {
    sonar::District d;
    sonar::district(d);
    char text[96];
    if (!d.has)
        std::snprintf(text, sizeof text, "zone  --");
    else if (d.level == 0)
        std::snprintf(text, sizeof text, "zone  %s", d.id);
    else if (d.cost == 0)
        std::snprintf(text, sizeof text, "zone  %s \xC2\xB7 lvl %d (max)", d.id, d.level);
    else
        std::snprintf(text, sizeof text, "zone  %s \xC2\xB7 lvl %d (%d/%d)", d.id, d.level, d.xp, d.cost);
    text_at(at, px, text);
    return at.y + std::floor(px * 1.8f);
}

void read_ini(long frame) {
    if (frame - g_ini_at < kIniEvery) return;
    g_ini_at = frame;
    static const std::wstring ini = log::path(L".ini");
    g_radius = static_cast<int>(GetPrivateProfileIntW(L"enemies", L"radius", 30, ini.c_str()));
    g_rows = static_cast<int>(GetPrivateProfileIntW(L"enemies", L"rows", 8, ini.c_str()));
    g_rows = std::min(g_rows, enemies::kMaxRows);
}

// "hiss", or the hash's first four digits.
void faction_label(uint32_t hash, char *out, size_t cap) {
    for (const auto &f : game::kFactions)
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
    for (const auto &n : game::kKinds)
        if (n.hash == k) {
            std::snprintf(out, cap, "%s%s", n.name, n.guess ? "?" : "");
            return;
        }
    if (whole)
        std::snprintf(out, cap, "%02x%02x%02x%02x%02x%02x%02x%02x", kind[0], kind[1], kind[2], kind[3], kind[4],
                      kind[5], kind[6], kind[7]);
    else
        std::snprintf(out, cap, "%02x%02x", kind[0], kind[1]);
}

bool shown(const enemies::Enemy &e) { return e.distance < 0.0f || e.distance <= static_cast<float>(g_radius); }

// A bar `fraction` full on a dark track.
void bar(ImVec2 at, float width, float height, float fraction, ImU32 fill) {
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    if (!(fraction >= 0.0f)) fraction = 0.0f;
    fraction = std::min(fraction, 1.0f);
    dl->AddRectFilled(at, ImVec2(at.x + width, at.y + height), IM_COL32(0, 0, 0, 150));
    if (fraction > 0.0f) dl->AddRectFilled(at, ImVec2(at.x + std::floor(width * fraction), at.y + height), fill);
}

// The panel's text sizes, smaller than the stats' so more rows fit: its lines, and the numbers beside each bar.
float panel_px(float px) { return std::floor(px * 0.75f); }
float number_px(float px) { return std::floor(px * 0.65f); }

// Under the stats block, the SRT shape: the count loaded and the count within the radius, then per enemy within it,
// nearest first, a line of what the walk read (faction, kind, level, distance) over its health bar, its shield as a
// thin white bar on top when it has one standing.
void enemy_panel(ImVec2 at, float stats_px, bool stale, long frame) {
    read_ini(frame);
    const float px = panel_px(stats_px);
    char text[96];
    if (stale) {
        text_at(at, px, "enemies  --");
        return;
    }
    enemies::read(g_list);
    int within = 0;
    for (int i = 0; i < g_list.count; i++)
        if (shown(g_list.rows[i])) within++;
    std::snprintf(text, sizeof text, "enemies  %d   shown %d", g_list.total, within);
    text_at(at, px, text);

    const float bar_w = std::floor(px * 8.0f), bar_h = std::floor(px * 0.45f), shield_h = std::floor(px * 0.3f);
    float y = at.y + std::floor(px * 1.5f);
    int drawn = 0;
    for (int i = 0; i < g_list.count && drawn < g_rows; i++) {
        const enemies::Enemy &e = g_list.rows[i];
        if (!shown(e)) continue;
        char faction[16], kind[40];
        faction_label(e.faction, faction, sizeof faction);
        kind_label(e.kind, false, kind, sizeof kind);
        if (e.distance < 0.0f)
            std::snprintf(text, sizeof text, "%s %s  lv%d", faction, kind, static_cast<int>(e.level));
        else
            std::snprintf(text, sizeof text, "%s %s  lv%d  %.0fm", faction, kind, static_cast<int>(e.level),
                          e.distance);
        text_at(ImVec2(at.x, y), px, text);
        y += px + std::floor(px * 0.2f);  // clear of the text, so the shield does not read as an underline
        const bool shielded = e.shield_max > 0 && e.shield_fraction > 0.0f;
        if (shielded) bar(ImVec2(at.x, y), bar_w, shield_h, e.shield_fraction, IM_COL32(240, 240, 240, 230));
        const float bar_y = y + shield_h + 1.0f;
        bar(ImVec2(at.x, bar_y), bar_w, bar_h, e.fraction, IM_COL32(210, 50, 40, 230));
        const int hp = static_cast<int>(e.fraction * e.max + 0.5f);
        if (shielded)
            std::snprintf(text, sizeof text, "%d / %d  +%d", hp, static_cast<int>(e.max),
                          static_cast<int>(e.shield_fraction * e.shield_max + 0.5f));
        else
            std::snprintf(text, sizeof text, "%d / %d", hp, static_cast<int>(e.max));
        const float npx = number_px(stats_px);
        text_at(ImVec2(at.x + bar_w + std::floor(px * 0.4f), std::floor(bar_y + (bar_h - npx) * 0.5f)), npx, text);
        y = bar_y + bar_h + std::floor(px * 0.5f);
        drawn++;
    }
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
        std::snprintf(row, sizeof row, "player at clip %g %g w %g -> screen %.3f %.3f", cx, cy, cw, cw ? cx / cw : 0.0f,
                      cw ? cy / cw : 0.0f);
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
    for (float size : {px, panel_px(px), number_px(px)}) ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, glyphs);
    injected_line(width, height, px);

    player::State p;
    long calls = 0;
    runner::player_state(p, &calls);
    const bool stale = runner_stale(calls, frame);
    const ImVec2 at(std::floor(width * 0.02f), std::floor(height * 0.30f));  // left edge, room below for the panel
    const float below = zone(ImVec2(at.x, stats(at, px, p, stale) - std::floor(px * 0.55f)), px);
    enemy_panel(ImVec2(at.x, below), px, stale, frame);
    labels(width, height, px, p, stale);
}

}  // namespace crsrt::ui
