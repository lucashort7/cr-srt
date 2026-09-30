// log.h — one timestamped line per call, appended to <basename>.log beside the DLL. Safe from any thread: a line is
// formatted by its caller and queued, and one writer thread appends the queue with a handle it keeps open. Until
// 2026-09-30 every line opened, wrote and closed the file on the caller's thread, and the runner once spent 635 ms in
// one call while WSL tailed the same file; a caller now pays a lock and a copy.
#pragma once

#include <windows.h>

#include <string>

namespace crsrt::log {

// Remembers this DLL's folder and basename (cr-srt-dev.dll -> "cr-srt-dev"); every other file it keeps is named after
// it, and starts the writer thread.
void init(HMODULE self);

// Flushes the queue, ends the writer thread and waits for it. Lines written after this go straight to the file on the
// caller's thread, the old way, so the detach line still lands. Called by the unloader before the DLL is unmapped.
void shutdown();

// <folder><basename><suffix>, e.g. path(L".cache").
std::wstring path(const wchar_t *suffix);

// wsprintf formatting: no floats. Lines past ~1000 characters are cut. Past kQueueMax queued lines the line is dropped
// and counted; the count is written when the queue drains.
void write(const char *fmt, ...);

}  // namespace crsrt::log
