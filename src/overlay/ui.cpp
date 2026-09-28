// ui.cpp
#include "overlay/ui.h"

#include "enemies.h"
#include "game.h"
#include "hooks/runner.h"
#include "log.h"

#include <windows.h>

#include <cfloat>
#include <cmath>
#include <cstdio>

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

// The stats block on the left, one row per number, health first. "--" when the runner cannot resolve the row or has
// stopped being called: a held-over value would read as live.
void stats(ImVec2 at, float px, bool got, bool stale, float frac, int max) {
    char text[64];
    if (!got || stale || !(frac >= 0.0f && frac < 100.0f) || max <= 0)
        std::snprintf(text, sizeof text, "health  --");
    else
        std::snprintf(text, sizeof text, "health  %d / %d  (%.1f%%)", static_cast<int>(frac * max + 0.5f), max,
                      frac * 100.0f);
    text_at(at, px, text);
}

void read_ini(long frame) {
    if (frame - g_ini_at < kIniEvery) return;
    g_ini_at = frame;
    static const std::wstring ini = log::path(L".ini");
    g_radius = static_cast<int>(GetPrivateProfileIntW(L"enemies", L"radius", 30, ini.c_str()));
    g_rows = static_cast<int>(GetPrivateProfileIntW(L"enemies", L"rows", 8, ini.c_str()));
    if (g_rows > enemies::kMaxRows) g_rows = enemies::kMaxRows;
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

bool shown(const enemies::Enemy &e) { return e.distance < 0.0f || e.distance <= static_cast<float>(g_radius); }

// A bar `fraction` full on a dark track.
void bar(ImVec2 at, float width, float height, float fraction, ImU32 fill) {
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    if (!(fraction >= 0.0f)) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
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
        char faction[16];
        faction_label(e.faction, faction, sizeof faction);
        if (e.distance < 0.0f)
            std::snprintf(text, sizeof text, "%s %02x%02x  lv%d", faction, e.kind[0], e.kind[1],
                          static_cast<int>(e.level));
        else
            std::snprintf(text, sizeof text, "%s %02x%02x  lv%d  %.0fm", faction, e.kind[0], e.kind[1],
                          static_cast<int>(e.level), e.distance);
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

}  // namespace

float font_px(float height) {
    float px = std::floor(height / 1440.0f * 26.0f);
    return px < 16.0f ? 16.0f : px;
}

void draw(float width, float height, float px, long frame) {
    // Every glyph the overlay can show, measured each frame at every size it draws. Glyphs bake the first time they are
    // drawn, and one first seen mid-game re-uploads the atlas with an unbounded wait on the game's queue; measured here
    // they bake with the atlas's first upload, and afterwards this is a cache lookup.
    static const char glyphs[] = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
                                 "abcdefghijklmnopqrstuvwxyz{|}~";
    for (float size : {px, panel_px(px), number_px(px)}) ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, glyphs);
    injected_line(width, height, px);

    float frac = 0.0f;
    int max = 0;
    long calls = 0;
    const bool got = runner::health(&frac, &max, &calls);
    const bool stale = runner_stale(calls, frame);
    const ImVec2 at(std::floor(width * 0.02f), std::floor(height * 0.30f));  // left edge, room below for the panel
    stats(at, px, got, stale, frac, max);
    enemy_panel(ImVec2(at.x, at.y + std::floor(px * 1.8f)), px, stale, frame);
}

}  // namespace crsrt::ui
