// guarded.cpp
#include "guarded.h"

#include <windows.h>

#include <atomic>

// rcx = dst, rdx = src, r8 = n (Win64). The one instruction that may fault is crsrt_guarded_movsb; the handler sends a
// fault there to crsrt_guarded_fail with the stack as the movsb left it, and both exits pop what the entry pushed.
asm(R"(
    .text
    .globl crsrt_guarded_copy
    .globl crsrt_guarded_movsb
    .globl crsrt_guarded_fail
crsrt_guarded_copy:
    push %rdi
    push %rsi
    mov %rcx, %rdi
    mov %rdx, %rsi
    mov %r8, %rcx
crsrt_guarded_movsb:
    rep movsb
    pop %rsi
    pop %rdi
    mov $1, %eax
    ret
crsrt_guarded_fail:
    pop %rsi
    pop %rdi
    xor %eax, %eax
    ret
)");

extern "C" {
int crsrt_guarded_copy(void *dst, const void *src, size_t n);
extern char crsrt_guarded_movsb[];
extern char crsrt_guarded_fail[];
}

namespace crsrt::guarded {

namespace {

std::atomic<void *> g_handler{nullptr};
// Threads inside on_exception: the handler runs for every exception of every thread of the game, and Windows does not
// wait for a running call when the handler is removed. remove() waits on this before the DLL may be unmapped.
volatile LONG g_inside;

LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    InterlockedIncrement(&g_inside);
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    LONG verdict = EXCEPTION_CONTINUE_SEARCH;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == STATUS_GUARD_PAGE_VIOLATION ||
         code == EXCEPTION_IN_PAGE_ERROR) &&
        e->ContextRecord->Rip == reinterpret_cast<DWORD64>(crsrt_guarded_movsb)) {
        e->ContextRecord->Rip = reinterpret_cast<DWORD64>(crsrt_guarded_fail);
        verdict = EXCEPTION_CONTINUE_EXECUTION;
    }
    InterlockedDecrement(&g_inside);
    return verdict;
}

}  // namespace

bool install() {
    if (g_handler.load()) return true;
    void *h = AddVectoredExceptionHandler(1, &on_exception);
    void *none = nullptr;
    if (h && !g_handler.compare_exchange_strong(none, h)) RemoveVectoredExceptionHandler(h);
    return g_handler.load() != nullptr;
}

void remove() {
    void *h = g_handler.exchange(nullptr);
    if (!h) return;
    RemoveVectoredExceptionHandler(h);
    // A thread can be inside the handler, or between ntdll's read of the entry and the handler's first instruction:
    // the count covers the first, the sleep the second, as hooks::release's does for a detour.
    do Sleep(200);
    while (g_inside != 0);
}

bool installed() { return g_handler.load() != nullptr; }

bool copy(void *dst, const void *src, size_t n) { return crsrt_guarded_copy(dst, src, n) != 0; }

}  // namespace crsrt::guarded
