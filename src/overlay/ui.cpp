// ui.cpp
#include "overlay/ui.h"

#include "hooks/runner.h"
#include "log.h"

#include <cfloat>
#include <cmath>
#include <cstdio>

#include "imgui.h"

namespace crsrt::ui {

namespace {

// Draws with no runner call in between before the health reads as stale: ~0.5 s at 60 fps.
constexpr long kStaleDraws = 30;

long g_seen_calls, g_still;  // the runner's count at the last draw, and draws since it last moved
bool g_stale_said;

// White on a drop shadow.
void text_at(ImVec2 at, float px, const char *text) {
    ImFont *font = ImGui::GetFont();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->AddText(font, px, ImVec2(at.x + 2.0f, at.y + 2.0f), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(font, px, at, IM_COL32(255, 255, 255, 235), text);
}

// Bottom centre, clear of the crosshair: the overlay is alive.
void injected_line(float w, float h, float px) {
    static const char text[] = "dll injected: yes";
    ImVec2 size = ImGui::GetFont()->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    text_at(ImVec2(std::floor((w - size.x) * 0.5f), std::floor(h - size.y - h * 0.03f)), px, text);
}

// The stats block on the left, one row per number, health first. "--" when the runner cannot resolve the row or has
// stopped being called: a held-over value would read as live.
void stats(float w, float h, float px, long frame) {
    float frac = 0.0f;
    int max = 0;
    long calls = 0;
    bool got = runner::health(&frac, &max, &calls);
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
    char text[64];
    if (!got || stale || !(frac >= 0.0f && frac < 100.0f) || max <= 0)
        std::snprintf(text, sizeof text, "health  --");
    else
        std::snprintf(text, sizeof text, "health  %d / %d  (%.1f%%)", static_cast<int>(frac * max + 0.5f), max,
                      frac * 100.0f);
    text_at(ImVec2(std::floor(w * 0.02f), std::floor(h * 0.60f)), px, text);  // left edge, above the game's own bars
}

}  // namespace

float font_px(float height) {
    float px = std::floor(height / 1440.0f * 26.0f);
    return px < 16.0f ? 16.0f : px;
}

void draw(float width, float height, float px, long frame) {
    // Every glyph the stats can show, measured each frame. Glyphs bake the first time they are drawn, and one first
    // seen mid-game re-uploads the atlas with an unbounded wait on the game's queue; measured here they bake with the
    // atlas's first upload, and afterwards this is a cache lookup.
    ImGui::GetFont()->CalcTextSizeA(px, FLT_MAX, 0.0f, "0123456789 /.%()-health");
    injected_line(width, height, px);
    stats(width, height, px, frame);
}

}  // namespace crsrt::ui
