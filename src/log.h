// log.h — one timestamped line per call, appended to <basename>.log beside the DLL. Safe from any thread: each line
// is one append-only WriteFile, so two threads logging at once both get their line.
#pragma once

#include <windows.h>

#include <string>

namespace crsrt::log {

// Remembers this DLL's folder and basename (cr-srt-dev.dll -> "cr-srt-dev"); every other file it keeps is named after
// it.
void init(HMODULE self);

// <folder><basename><suffix>, e.g. path(L".cache").
std::wstring path(const wchar_t *suffix);

// wsprintf formatting: no floats. Lines past ~1000 characters are cut.
void write(const char *fmt, ...);

}  // namespace crsrt::log
