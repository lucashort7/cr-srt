// memory.cpp
#include "memory.h"

#include <windows.h>

namespace crsrt::mem {

namespace {
bool query(const void *p, MEMORY_BASIC_INFORMATION &mbi) {
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) == 0) return false;
    return mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && mbi.Protect != PAGE_NOACCESS;
}
}  // namespace

bool readable(const void *p, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!query(p, mbi)) return false;
    return static_cast<const char *>(p) + len <= static_cast<const char *>(mbi.BaseAddress) + mbi.RegionSize;
}

size_t readable_run(const void *p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!query(p, mbi)) return 0;
    return static_cast<size_t>(static_cast<const char *>(mbi.BaseAddress) + mbi.RegionSize -
                               static_cast<const char *>(p));
}

bool copy(const void *p, void *out, size_t n) {
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
