// memory.cpp
#include "memory.h"

#include "guarded.h"

#include <windows.h>

namespace crsrt::mem {

// VirtualQuery's own time: it takes the process's address-space lock, the one the game's allocator holds while it
// spawns, so its wait is the suspect for a stutter (AKTH-0111, 2026-09-30). Read and reset by query_meter.
namespace {
volatile LONG64 g_vq_calls, g_vq_us, g_vq_max_us;
const LONG64 g_hz = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}();

bool query(const void *p, MEMORY_BASIC_INFORMATION &mbi) {
    if (!p) return false;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    const SIZE_T got = VirtualQuery(p, &mbi, sizeof mbi);
    QueryPerformanceCounter(&b);
    const LONG64 us = (b.QuadPart - a.QuadPart) * 1000000 / g_hz;
    InterlockedIncrement64(&g_vq_calls);
    InterlockedAdd64(&g_vq_us, us);
    LONG64 seen = g_vq_max_us;
    while (us > seen && InterlockedCompareExchange64(&g_vq_max_us, us, seen) != seen) seen = g_vq_max_us;
    if (got == 0) return false;
    return mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && mbi.Protect != PAGE_NOACCESS;
}
}  // namespace

void query_meter(long long *calls, long long *avg_us, long long *max_us) {
    const LONG64 c = InterlockedExchange64(&g_vq_calls, 0), t = InterlockedExchange64(&g_vq_us, 0);
    *max_us = InterlockedExchange64(&g_vq_max_us, 0);
    *calls = c;
    *avg_us = c ? t / c : 0;
}

// Proved by reading, not by asking: the guarded copy once its handler is in, else ReadProcessMemory, both failing
// cleanly on a page that is not there. VirtualQuery here cost 212 us on average under the game's spawning (9 000 calls
// in 20 s, 2026-09-30 18:32), a queue on the address-space lock the allocator also takes; ReadProcessMemory on this
// process queued the same way, 1-6.5 ms for a row memcpy read in 0-3 µs (2026-10-01).
bool readable(const void *p, size_t len) {
    if (!p || !len) return false;
    alignas(16) char scratch[4096];
    const char *at = static_cast<const char *>(p);
    size_t left = len;
    while (left) {
        const size_t n = left < sizeof scratch ? left : sizeof scratch;
        if (!copy(at, scratch, n)) return false;
        at += n;
        left -= n;
    }
    return true;
}

size_t readable_run(const void *p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!query(p, mbi)) return 0;
    return static_cast<size_t>(static_cast<const char *>(mbi.BaseAddress) + mbi.RegionSize -
                               static_cast<const char *>(p));
}

bool copy(const void *p, void *out, size_t n) {
    if (guarded::installed()) return guarded::copy(out, p, n);
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

bool is_code(const void *p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) == 0 || mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

void owner_of(const void *p, char *out, size_t cap) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(p), &m) ||
        !GetModuleFileNameW(m, path, MAX_PATH)) {
        lstrcpynA(out, "?", static_cast<int>(cap));
        return;
    }
    wchar_t *base = path + lstrlenW(path);
    while (base > path && base[-1] != L'\\') base--;
    char line[320];
    wsprintfA(line, "%ls+%lx", base,
              static_cast<unsigned long>(static_cast<const char *>(p) - reinterpret_cast<const char *>(m)));
    lstrcpynA(out, line, static_cast<int>(cap));
}

}  // namespace crsrt::mem
