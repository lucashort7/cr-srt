// registry.h — every hook cr-srt installs, in one book: MinHook's create and enable, the calls in flight the unloader
// waits on before unmapping, and the time each detour spends of the game's thread. A detour opens a Call on entry; the
// unloader asks the registry, not each hook. The env tray of a system's context is read here too, since every hooked
// system reads its parameters through it.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace crsrt::hooks {

// One hooked function. A static of the module that owns the detour; the registry keeps its address once installed.
class Hook {
   public:
    // A hook in an array, named when installed.
    constexpr Hook() = default;
    explicit constexpr Hook(const char *name) : name_(name) {}
    Hook(const Hook &) = delete;
    Hook &operator=(const Hook &) = delete;

    // Hooks `target` with `detour` and enables it; the hook joins the registry. A name given replaces the one it was
    // built with. MinHook must be initialised. False with the reason logged, and nothing hooked.
    bool install(void *target, void *detour, const char *name = nullptr);
    // install() at exe + offset.
    bool install(HMODULE exe, uintptr_t offset, void *detour, const char *name = nullptr);

    // MinHook's trampoline: the function as it was.
    template <class Fn>
    Fn original() const {
        return reinterpret_cast<Fn>(original_);
    }
    const char *name() const { return name_; }
    bool hooked() const { return hooked_; }
    long inflight() const { return inflight_; }

   private:
    friend class Call;
    friend void log_meters();
    const char *name_ = "?";
    void *original_ = nullptr;
    bool hooked_ = false;
    volatile LONG inflight_ = 0;
    // since the last log_meters: calls, the detour's own microseconds summed, the most one call took
    volatile LONG64 calls_ = 0, sum_us_ = 0, max_us_ = 0;
};

// One call inside a detour, opened first thing. Counts the call in flight until the scope ends, and times the detour's
// own work on the calling thread until before_original() or the scope's end, whichever is first: the original's time is
// the game's. Any thread, several at once.
class Call {
   public:
    explicit Call(Hook &hook);
    ~Call();
    Call(const Call &) = delete;
    Call &operator=(const Call &) = delete;

    // The unloader has started: a detour then does nothing of its own and calls the original.
    bool stopping() const;
    void before_original();
    // A detour that tail-calls its original ([[gnu::musttail]]) opens its Call in an inner scope that ends before the
    // call: the thread is then in the game's code, which the unloader does not unmap.

   private:
    Hook *hook_;
    int64_t from_us_;
    bool timed_ = false;
};

// Hooks installed, in order.
int count();
// Calls in flight across every hook.
long inflight();
// "runner 1, present 1": each hook with a call in flight, for the unloader's log.
void describe_inflight(char *out, size_t cap);
// Disables every hook, retrying ten times 100 ms apart. False when MinHook still refuses.
bool disable_all();
// The unloader's part: every hook disabled, then a wait with no ceiling for the calls in flight to leave, ending on a
// zero read after a sleep. A thread that entered before the disable counts itself within a few instructions, and the
// render thread can sit inside the original Present for a frame or more (vsync, a minimized window) before it returns
// into its detour; with the hooks off nothing new enters. False when the disable failed: the DLL must stay mapped.
bool release();
// One line per hook called since the last: its calls and its own average and max microseconds; resets.
void log_meters();

// A system's env tray, *(ctx + kEnvTray), copied. False when the copy fails or the tray is null.
bool tray(const void *ctx, const uint8_t **out);
// The env at byte `offset` of a system's tray, game.h's slot constants (kHudVisFactDictionary = 3 * 8):
// *(*(ctx + kEnvTray) + offset), each step a copy. False when a step fails or lands on null.
bool env(const void *ctx, uintptr_t offset, const uint8_t **out);
// The tray's first `slots` slots to the log, each pointer with its owner and its first 16 bytes as four floats and as
// hex, and to envwatch's list of envs. For a system's first call: which slot holds which parameter.
void log_tray(const char *who, const void *ctx, int slots);

}  // namespace crsrt::hooks
