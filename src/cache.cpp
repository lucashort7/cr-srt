// cache.cpp — the line: "cr-srt-cache <version> <fingerprint> getplayer=<hex> getp_lookup=<hex> runner=<hex>
// sonar=<hex>".
#include "cache.h"

#include "log.h"

#include <cstring>

namespace crsrt::cache {

namespace {

constexpr int kVersion = 2;  // 2: sonar=
constexpr char kTag[] = "cr-srt-cache ";

unsigned long hi32(uint64_t x) { return static_cast<unsigned long>(x >> 32); }
unsigned long lo32(uint64_t x) { return static_cast<unsigned long>(x & 0xffffffffUL); }

// ascii hex -> value; *end points at the first non-hex character
uint64_t hex64(const char *s, const char **end) {
    uint64_t v = 0;
    for (;; s++) {
        char c = *s;
        int d = (c >= '0' && c <= '9')   ? c - '0'
                : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                         : -1;
        if (d < 0) break;
        v = v * 16 + static_cast<uint64_t>(d);
    }
    *end = s;
    return v;
}

}  // namespace

uint64_t fingerprint(HMODULE exe) {
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exe);
    const auto *nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64 *>(reinterpret_cast<const uint8_t *>(exe) + dos->e_lfanew);
    const uint32_t fields[3] = {nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
                                nt->OptionalHeader.CheckSum};
    const auto *p = reinterpret_cast<const uint8_t *>(fields);
    uint64_t h = 0xcbf29ce484222325ULL;
    // The hash walks the three fields byte by byte, so sizeof over the array is the count meant.
    // NOLINTNEXTLINE(bugprone-sizeof-expression)
    for (size_t i = 0; i < sizeof fields; i++) {
        // The analyzer does not follow the byte pointer back to the initialized array.
        // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

Status load(uint64_t fp, Offsets &out) {
    HANDLE h = CreateFileW(log::path(L".cache").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Status::Absent;
    char buf[1024];
    DWORD n = 0;
    ReadFile(h, buf, sizeof buf - 1, &n, nullptr);
    CloseHandle(h);
    buf[n] = 0;

    if (std::strncmp(buf, kTag, sizeof kTag - 1) != 0) return Status::BadVersion;  // not our line at all
    const char *s = buf + sizeof kTag - 1, *e;
    if (hex64(s, &e) != kVersion || *e != ' ') return Status::BadVersion;
    if (hex64(e + 1, &e) != fp || *e != ' ') return Status::BadFingerprint;

    Offsets found;
    for (s = e; *s;) {  // key=hex pairs
        while (*s == ' ') s++;
        const char *key = s;
        while (*s && *s != '=' && *s != ' ' && *s != '\r' && *s != '\n') s++;
        if (*s != '=') break;
        size_t len = static_cast<size_t>(s - key);
        uint64_t v = hex64(s + 1, &s);
        if (len == 9 && std::strncmp(key, "getplayer", 9) == 0) found.get_player = v;
        if (len == 11 && std::strncmp(key, "getp_lookup", 11) == 0) found.get_player_lookup = v;
        if (len == 6 && std::strncmp(key, "runner", 6) == 0) found.runner = v;
        if (len == 5 && std::strncmp(key, "sonar", 5) == 0) found.sonar = v;
    }
    if (!found.get_player || !found.runner) return Status::BadVersion;  // a line missing a key is solved again
    out = found;
    return Status::Hit;
}

void save(uint64_t fp, const Offsets &o) {
    HANDLE h = CreateFileW(log::path(L".cache").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log::write("cache: cannot write %ls", log::path(L".cache").c_str());
        return;
    }
    char line[256];
    int n = wsprintfA(line, "%s%d %08lx%08lx getplayer=%lx getp_lookup=%lx runner=%lx sonar=%lx\r\n", kTag, kVersion,
                      hi32(fp), lo32(fp), static_cast<unsigned long>(o.get_player),
                      static_cast<unsigned long>(o.get_player_lookup), static_cast<unsigned long>(o.runner),
                      static_cast<unsigned long>(o.sonar));
    DWORD written;
    WriteFile(h, line, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(h);
}

const char *describe(Status status) {
    switch (status) {
        case Status::Hit: return "hit";
        case Status::Absent: return "absent";
        case Status::BadVersion: return "miss (version)";
        case Status::BadFingerprint: return "miss (fingerprint)";
    }
    return "?";
}

}  // namespace crsrt::cache
