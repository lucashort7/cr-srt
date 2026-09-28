// log.cpp
#include "log.h"

#include <cstdarg>

namespace crsrt::log {

namespace {
wchar_t g_dir[MAX_PATH];  // with its trailing backslash
wchar_t g_name[64];
}  // namespace

void init(HMODULE self) {
    DWORD n = GetModuleFileNameW(self, g_dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    wchar_t *p = g_dir + n;
    while (p > g_dir && *p != L'\\') --p;
    lstrcpynW(g_name, p + 1, 64);
    for (wchar_t *q = g_name; *q; q++)
        if (*q == L'.') {
            *q = 0;
            break;
        }
    p[1] = 0;
}

std::wstring path(const wchar_t *suffix) { return std::wstring(g_dir) + g_name + suffix; }

void write(const char *fmt, ...) {
    HANDLE h = CreateFileW(path(L".log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[1100];  // wvsprintfA writes at most 1024
    int n = wsprintfA(buf, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD written;
    WriteFile(h, buf, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(h);
}

}  // namespace crsrt::log
