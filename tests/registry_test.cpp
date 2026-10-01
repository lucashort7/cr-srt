// registry_test.cpp — the registry over a real MinHook hook on a function of this exe, and the env tray over a buffer
// laid out as a system's context.
#include "hooks/registry.h"
#include "check.h"
#include "game.h"

#include "MinHook.h"

#include <cstdint>
#include <cstring>

using namespace crsrt;

namespace {

using AddFn = int (*)(int, int);

// Not inlined, not folded: the hook needs a real function to patch.
[[gnu::noinline]] int add(int a, int b) {
    asm volatile("");
    return a + b;
}

hooks::Hook g_add("test_add");
long g_inflight_seen = -1;

int hk_add(int a, int b) {
    hooks::Call call(g_add);
    g_inflight_seen = hooks::inflight();
    if (call.stopping()) return g_add.original<AddFn>()(a, b);
    call.before_original();
    return g_add.original<AddFn>()(a, b) + 1000;
}

// Called through a pointer the optimiser cannot see through, so the call goes to the patched bytes.
AddFn volatile g_target = &add;

}  // namespace

TEST(a_hook_counts_its_call_in_flight_and_meters_it) {
    CHECK(MH_Initialize() == MH_OK);
    CHECK(g_add.install(reinterpret_cast<void *>(&add), reinterpret_cast<void *>(&hk_add)));
    CHECK(g_add.hooked());
    CHECK(hooks::count() == 1);
    CHECK(g_target(2, 3) == 1005);
    CHECK(g_inflight_seen == 1);
    CHECK(hooks::inflight() == 0);
    char busy[64];
    hooks::describe_inflight(busy, sizeof busy);
    CHECK(busy[0] == 0);
    CHECK(hooks::disable_all());
    CHECK(g_target(2, 3) == 5);
    CHECK(MH_Uninitialize() == MH_OK);
}

TEST(a_hook_on_nothing_installs_nothing) {
    CHECK(MH_Initialize() == MH_OK);
    static hooks::Hook none("test_none");
    CHECK(!none.install(nullptr, reinterpret_cast<void *>(&hk_add)));
    CHECK(!none.hooked());
    CHECK(MH_Uninitialize() == MH_OK);
}

TEST(a_call_is_in_flight_for_its_scope) {
    static hooks::Hook h("test_scope");
    {
        const hooks::Call call(h);
        CHECK(h.inflight() == 1);
    }
    CHECK(h.inflight() == 0);
}

// ctx + kEnvTray holds the tray, tray + offset the env.
TEST(the_env_tray_reads_a_slot) {
    const uint8_t a = 1, c = 3;
    const uint8_t *envs[3] = {&a, nullptr, &c};
    alignas(8) uint8_t ctx[game::kEnvTray + 8] = {};
    const auto *tray_at = reinterpret_cast<const uint8_t *>(envs);
    std::memcpy(ctx + game::kEnvTray, &tray_at, 8);
    const uint8_t *tray = nullptr, *env = nullptr;
    CHECK(hooks::tray(ctx, &tray));
    CHECK(tray == tray_at);
    CHECK(hooks::env(ctx, 2 * 8, &env));
    CHECK(env == &c);
    CHECK(!hooks::env(ctx, 1 * 8, &env));  // a null env
    CHECK(!hooks::env(nullptr, 0, &env));
    std::memset(ctx + game::kEnvTray, 0, 8);
    CHECK(!hooks::tray(ctx, &tray));  // a null tray
    CHECK(!hooks::env(ctx, 0, &env));
}
