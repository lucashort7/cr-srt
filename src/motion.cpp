// motion.cpp
#include "motion.h"

#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace crsrt::motion {

namespace {

constexpr int kSpan = 6;              // runner calls a derived velocity spans, ~0.2 s at 30 calls a second
constexpr long kWindow = 30;          // runner calls per peaks window, ~1 s
constexpr int64_t kGapUs = 200000;    // two calls further apart than this straddle a pause: the span starts over
constexpr float kTeleport = 100.0f;   // metres a second between two calls: no movement, a teleport
constexpr float kTeleportMin = 3.0f;  // metres: a shorter jump is a step over two calls that came close together
constexpr float kFast = 8.0f;         // a game speed past a jog is worth a peaks line
constexpr float kClimb = 1.0f;        // so is a vertical speed past this, either way
constexpr float kTilted = 0.9f;       // the body's up axis below this in world Y: gravity turned, or the body did

struct Sample {
    float p[3];
    int64_t us;
};
Sample g_ring[kSpan + 1];
int g_held, g_head;  // samples in the ring, and the slot the next one goes to
bool g_have;
float g_h, g_v;  // the last derived velocity
long g_teleport_at = -kWindow;
bool g_tilted;  // the last up axis logged was tilted

// The body's up axis in world coordinates: the quaternion (x y z w) applied to (0, 1, 0).
void up_axis(const float q[4], float out[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    out[0] = 2.0f * (x * y - w * z);
    out[1] = 1.0f - 2.0f * (x * x + z * z);
    out[2] = 2.0f * (y * z + w * x);
}

// One window's extremes: the game's speed, the derived ones, the height; and, at the sample of the largest derived
// vertical speed, the game's speed and its vector beside the derived pair.
struct Peaks {
    bool any = false;
    float speed = 0, h = 0, v_min = 0, v_max = 0, y_min = 0, y_max = 0;
    float at_v = 0, at_v_speed = -1, at_v_h = 0;
    bool at_v_has_game = false;
    float at_v_game_h = 0, at_v_game_v = 0;
    float up_min = 2, up[3] = {};  // the most tilted up axis of the window, by its world Y
} g_peaks;

const int64_t g_qpc_hz = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}();

int64_t now_us() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000000 / g_qpc_hz;
}

void restart() {
    g_held = g_head = 0;
    g_have = false;
}

void push(const float p[3], int64_t us) {
    Sample &s = g_ring[g_head];
    s.p[0] = p[0], s.p[1] = p[1], s.p[2] = p[2];
    s.us = us;
    g_head = (g_head + 1) % (kSpan + 1);
    if (g_held < kSpan + 1) g_held++;
}

const Sample &newest() { return g_ring[(g_head + kSpan) % (kSpan + 1)]; }

void note(const player::State &s) {
    Peaks &k = g_peaks;
    const float y = s.position[1];
    if (!k.any) {
        k = {};
        k.any = true;
        k.y_min = k.y_max = y;
    }
    // The kept value goes first: std::min and std::max return their first argument when the other is NaN.
    k.y_min = std::min(k.y_min, y);
    k.y_max = std::max(k.y_max, y);
    if (s.has_speed && s.speed > k.speed) k.speed = s.speed;
    float up[3];
    up_axis(s.rotation, up);
    if (up[1] < k.up_min) {
        k.up_min = up[1];
        k.up[0] = up[0], k.up[1] = up[1], k.up[2] = up[2];
    }
    if (!g_have) return;
    k.h = std::max(k.h, g_h);
    k.v_min = std::min(k.v_min, g_v);
    k.v_max = std::max(k.v_max, g_v);
    if (std::fabs(g_v) >= std::fabs(k.at_v)) {
        k.at_v = g_v;
        k.at_v_speed = s.has_speed ? s.speed : -1.0f;
        k.at_v_h = g_h;
        k.at_v_has_game = s.has_velocity;
        k.at_v_game_h = s.h;
        k.at_v_game_v = s.velocity[1];
    }
}

void close_window(long calls) {
    const Peaks &k = g_peaks;
    const bool tilted = k.any && k.up_min < kTilted;
    if (tilted != g_tilted || (tilted && k.speed > kFast)) {
        char line[160];
        std::snprintf(line, sizeof line, "player: up axis %.3g %.3g %.3g, %s (call %ld)", k.up[0], k.up[1], k.up[2],
                      tilted ? "tilted" : "upright again", calls);
        log::write("%s", line);
        g_tilted = tilted;
    }
    if (k.any && (k.speed > kFast || k.v_max > kClimb || k.v_min < -kClimb)) {
        char line[360], game[48] = "--";
        if (k.at_v_has_game) std::snprintf(game, sizeof game, "h %.4g v %.4g", k.at_v_game_h, k.at_v_game_v);
        std::snprintf(line, sizeof line,
                      "player: 1 s peaks: speed %.4g, calc h %.4g v %.4g .. %.4g, y %.6g .. %.6g; at the largest "
                      "|v|: speed %.4g, calc h %.4g v %.4g, game %s (call %ld)",
                      k.speed, k.h, k.v_min, k.v_max, k.y_min, k.y_max, k.at_v_speed, k.at_v_h, k.at_v, game, calls);
        log::write("%s", line);
    }
    g_peaks = {};
}

}  // namespace

void sample(const player::State &s, long calls) {
    if (calls % kWindow == 0) close_window(calls);
    if (!s.has_position) {
        restart();
        return;
    }
    const int64_t us = now_us();
    if (g_held) {
        const Sample &last = newest();
        const int64_t dt = us - last.us;
        const float dx = s.position[0] - last.p[0], dy = s.position[1] - last.p[1], dz = s.position[2] - last.p[2];
        const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dt <= 0 || dt > kGapUs) {
            if (dt > kGapUs && step > kTeleportMin && calls - g_teleport_at >= kWindow) {
                char line[160];
                std::snprintf(line, sizeof line,
                              "player: moved %.4g m across a %.3g s pause of the runner, read as a "
                              "teleport (call %ld)",
                              step, static_cast<float>(dt) / 1e6f, calls);
                log::write("%s", line);
                g_teleport_at = calls;
            }
            restart();
        } else if (step > kTeleportMin && step / (static_cast<float>(dt) / 1e6f) > kTeleport) {
            if (calls - g_teleport_at >= kWindow) {
                char line[160];
                std::snprintf(line, sizeof line,
                              "player: moved %.4g m in %.4g ms between two runner calls, read "
                              "as a teleport (call %ld)",
                              step, static_cast<float>(dt) / 1e3f, calls);
                log::write("%s", line);
                g_teleport_at = calls;
            }
            restart();
        }
    }
    push(s.position, us);
    if (g_held == kSpan + 1) {
        const Sample &old = g_ring[g_head];  // the slot the next push overwrites: the oldest
        const float dt = static_cast<float>(us - old.us) / 1e6f;
        const float dx = s.position[0] - old.p[0], dy = s.position[1] - old.p[1], dz = s.position[2] - old.p[2];
        g_h = std::sqrt(dx * dx + dz * dz) / dt;
        g_v = dy / dt;
        g_have = dt > 0.0f;
    }
    note(s);
}

void text(char *out, size_t cap) {
    if (g_have)
        std::snprintf(out, cap, "calc h %.4g v %.4g", g_h, g_v);
    else
        std::snprintf(out, cap, "calc --");
}

}  // namespace crsrt::motion
