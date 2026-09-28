// lifecycle.h — what the boot leaves behind for the unloader, and the one flag every hook reads while unloading.
#pragma once

#include <atomic>

namespace crsrt::lifecycle {

// Set once by the unloader, never cleared: hooks then only pass calls through.
inline std::atomic<bool> stopping{false};

// Set by the boot (dllmain.cpp), read by the unloader once the boot thread has ended.
inline std::atomic<bool> minhook_ready{false};
inline std::atomic<bool> runner_hooked{false};
inline std::atomic<bool> present_hooked{false};
inline std::atomic<bool> damage_hooked{false};  // the probe, only with [probe] damage=1

}  // namespace crsrt::lifecycle
