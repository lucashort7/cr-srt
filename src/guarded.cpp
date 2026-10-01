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

LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if ((code != EXCEPTION_ACCESS_VIOLATION && code != STATUS_GUARD_PAGE_VIOLATION) ||
        e->ContextRecord->Rip != reinterpret_cast<DWORD64>(crsrt_guarded_movsb))
        return EXCEPTION_CONTINUE_SEARCH;
    e->ContextRecord->Rip = reinterpret_cast<DWORD64>(crsrt_guarded_fail);
    return EXCEPTION_CONTINUE_EXECUTION;
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
    if (void *h = g_handler.exchange(nullptr)) RemoveVectoredExceptionHandler(h);
}

bool installed() { return g_handler.load() != nullptr; }

bool copy(void *dst, const void *src, size_t n) { return crsrt_guarded_copy(dst, src, n) != 0; }

}  // namespace crsrt::guarded
