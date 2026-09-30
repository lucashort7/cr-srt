// log.cpp
#include "log.h"

#include <cstdarg>
#include <cstring>
#include <vector>

namespace crsrt::log {

namespace {
constexpr size_t kLineMax = 1100;  // wvsprintfA writes at most 1024
constexpr size_t kQueueMax = 4096;
constexpr DWORD kFlushMs = 100;

wchar_t g_dir[MAX_PATH];  // with its trailing backslash
wchar_t g_name[64];

SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<std::string> g_queue;  // guarded by g_lock
long g_dropped;                    // guarded by g_lock
HANDLE g_wake;                     // set by write, waited on by the writer
HANDLE g_thread;
volatile LONG g_running;  // 1 while the writer thread takes the queue

void append(HANDLE h, const char *text, size_t n) {
    DWORD written = 0;
    WriteFile(h, text, static_cast<DWORD>(n), &written, nullptr);
}

HANDLE open() {
    return CreateFileW(path(L".log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// Takes what is queued and writes it, one WriteFile per batch. Returns how many lines it wrote.
size_t drain(HANDLE h) {
    std::vector<std::string> batch;
    long dropped = 0;
    AcquireSRWLockExclusive(&g_lock);
    batch.swap(g_queue);
    dropped = g_dropped;
    g_dropped = 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (batch.empty() && !dropped) return 0;
    std::string text;
    for (const std::string &line : batch) text += line;
    if (dropped) {
        char note[96];
        int n = wsprintfA(note, "log: %ld lines dropped, the queue was full\r\n", dropped);
        text.append(note, static_cast<size_t>(n));
    }
    append(h, text.data(), text.size());
    return batch.size();
}

DWORD WINAPI writer(LPVOID) {
    HANDLE h = open();
    while (g_running) {
        WaitForSingleObject(g_wake, kFlushMs);
        if (h == INVALID_HANDLE_VALUE) h = open();
        if (h != INVALID_HANDLE_VALUE) drain(h);
    }
    if (h != INVALID_HANDLE_VALUE) {
        drain(h);
        CloseHandle(h);
    }
    return 0;
}

void format(char *buf, int &n, const char *fmt, va_list ap) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    n = wsprintfA(buf, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    n += wvsprintfA(buf + n, fmt, ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
}
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
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_running = 1;
    g_thread = CreateThread(nullptr, 0, writer, nullptr, 0, nullptr);
    if (!g_thread) g_running = 0;
}

void shutdown() {
    if (!g_thread) return;
    InterlockedExchange(&g_running, 0);
    SetEvent(g_wake);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    CloseHandle(g_wake);
    g_thread = nullptr;
    g_wake = nullptr;
}

std::wstring path(const wchar_t *suffix) { return std::wstring(g_dir) + g_name + suffix; }

void write(const char *fmt, ...) {
    char buf[kLineMax];
    int n = 0;
    // va_start is what initializes it, and va_list is a pointer only on this target.
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    va_list ap;
    va_start(ap, fmt);
    format(buf, n, fmt, ap);
    va_end(ap);
    if (!g_running) {  // before init or after shutdown: the old synchronous append
        HANDLE h = open();
        if (h == INVALID_HANDLE_VALUE) return;
        append(h, buf, static_cast<size_t>(n));
        CloseHandle(h);
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (g_queue.size() < kQueueMax)
        g_queue.emplace_back(buf, static_cast<size_t>(n));
    else
        g_dropped++;
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(g_wake);
}

}  // namespace crsrt::log
