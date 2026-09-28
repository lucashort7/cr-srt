// snapshot.cpp
#include "probe/snapshot.h"

#include "ecs.h"
#include "game.h"
#include "log.h"
#include "memory.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace crsrt::snapshot {

namespace {

// The file, little-endian:
//   "CRSNAP03" | u64 world | u32 runner calls | u32 slots scanned | u32 world length | the world's first bytes
//   per archetype whose tables read: u32 'ARCH' | i32 index | u64 chunk | u32 type count | u32 blob length
//     | 32 bytes of its hashes entry (world+8+(index+0xc22)*32) | 32 bytes of its offsets entry (world+8+index*32+0x18450)
//     | u32 hashes[count] | u32 offsets[count] | u32 n | u32 first[n]: the array the hashes entry's +0 points at,
//     n from its +0x18 (a few more than count, unread) | blob: the chunk's first bytes, its header included
//   u32 'END!'
constexpr uint32_t kArchTag = 0x48435241;  // "ARCH"
constexpr uint32_t kEndTag = 0x21444e45;   // "END!"
constexpr uint32_t kBlobSlack = 0x400;     // past the last column's offset: that column's rows, width unknown
constexpr uint32_t kBlobMax = 4u << 20;    // one chunk, at most
constexpr long kFlagEvery = 15;            // runner calls between looks for the flag, ~0.5 s
constexpr uint32_t kWorldMax = 2u << 20;   // the world object: the chunk slots and the archetype entries end ~0x9c450

bool g_key_down;
std::vector<uint8_t> g_buf;

void put(const void *p, size_t n) {
    const auto *b = static_cast<const uint8_t *>(p);
    g_buf.insert(g_buf.end(), b, b + n);
}

template <class T>
void put(T v) {
    put(&v, sizeof v);
}

// Copies n bytes at p to the end of the buffer through ReadProcessMemory, which fails instead of faulting when a
// worker thread frees the memory between the check and the copy. False leaves the buffer as it was.
bool put_copy(const void *p, size_t n) {
    const size_t at = g_buf.size();
    g_buf.resize(at + n);
    SIZE_T got = 0;
    if (ReadProcessMemory(GetCurrentProcess(), p, g_buf.data() + at, n, &got) && got == n) return true;
    g_buf.resize(at);
    return false;
}

// 32 bytes at p, zeros when unreadable.
void put_entry(const uint8_t *p) {
    if (!put_copy(p, 32)) g_buf.resize(g_buf.size() + 32, 0);
}

void take(const uint8_t *world, long calls, const char *why) {
    DWORD t0 = GetTickCount();
    g_buf.clear();
    g_buf.reserve(16u << 20);
    put("CRSNAP03", 8);
    put(reinterpret_cast<uint64_t>(world));
    put(static_cast<uint32_t>(calls));
    put(static_cast<uint32_t>(game::kArchetypeMax));
    const size_t world_run = mem::readable_run(world);
    uint32_t world_len = static_cast<uint32_t>(world_run < kWorldMax ? world_run : kWorldMax);
    const size_t world_at = g_buf.size();
    put(world_len);
    if (!put_copy(world, world_len)) {
        world_len = 0;
        std::memcpy(g_buf.data() + world_at, &world_len, 4);
    }

    const auto *slots = reinterpret_cast<const uint64_t *>(world + game::kChunkSlotBias * 8);
    int archs = 0, blobs = 0, capped = 0, lost = 0;
    for (int64_t a = 0; a < game::kArchetypeMax;) {
        int64_t end = a + static_cast<int64_t>(mem::readable_run(&slots[a]) / 8);
        if (end == a) break;
        if (end > game::kArchetypeMax) end = game::kArchetypeMax;
        for (; a < end; a++) {
            ecs::Archetype arch;
            if (!ecs::archetype(world, a, arch) || !arch.count) continue;
            const uint64_t chunk = slots[a];
            uint32_t blob = 0;
            if (chunk && ecs::type_index(arch, game::kHealth) >= 0) {
                uint32_t last = 0;
                for (uint32_t i = 0; i < arch.count; i++)
                    if (arch.offsets[i] > last) last = arch.offsets[i];
                size_t want = static_cast<size_t>(last) + kBlobSlack;
                if (want > kBlobMax) want = kBlobMax, capped++;
                const size_t run = mem::readable_run(reinterpret_cast<const void *>(chunk));
                blob = static_cast<uint32_t>(run < want ? run : want);
            }
            put(kArchTag);
            put(static_cast<int32_t>(a));
            put(chunk);
            put(arch.count);
            const size_t blob_at = g_buf.size();
            put(blob);
            put_entry(world + 8 + (a + game::kTypeEntryBias) * 32);
            put_entry(world + 8 + a * 32 + game::kOffsetsBase);
            put(arch.hashes, arch.count * 4);
            put(arch.offsets, arch.count * 4);
            const uint8_t *entry = world + 8 + (a + game::kTypeEntryBias) * 32;
            const auto *first = *reinterpret_cast<const uint8_t *const *>(entry);
            uint32_t n = *reinterpret_cast<const uint32_t *>(entry + 0x18);
            if (n > 4096) n = 0;
            const size_t n_at = g_buf.size();
            put(n);
            if (n && !put_copy(first, n * 4)) {
                const uint32_t none = 0;
                std::memcpy(g_buf.data() + n_at, &none, 4);
            }
            if (blob) {
                if (put_copy(reinterpret_cast<const void *>(chunk), blob)) {
                    blobs++;
                } else {
                    const uint32_t none = 0;
                    std::memcpy(g_buf.data() + blob_at, &none, 4);
                    lost++;
                }
            }
            archs++;
        }
    }
    put(kEndTag);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t suffix[48];
    wsprintfW(suffix, L".snap-%04d%02d%02d-%02d%02d%02d-%03d.bin", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
              t.wSecond, t.wMilliseconds);
    const std::wstring file = log::path(suffix);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, g_buf.data(), static_cast<DWORD>(g_buf.size()), &written,
                                                     nullptr) && written == g_buf.size();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    log::write("snapshot (%s, call %ld): world %lu KB, %d archetypes, %d chunks with Health (%d capped at 4 MB, "
               "%d unreadable), %lu KB in %lu ms -> %ls%s",
               why, calls, static_cast<unsigned long>(world_len / 1024), archs, blobs, capped, lost,
               static_cast<unsigned long>(g_buf.size() / 1024),
               GetTickCount() - t0, file.c_str(), ok ? "" : " WRITE FAILED");
    g_buf.clear();
    g_buf.shrink_to_fit();
}

}  // namespace

void tick(void *world, long calls) {
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const char *why = down && !g_key_down ? "F9" : nullptr;
    g_key_down = down;
    if (!why && calls % kFlagEvery == 0) {
        static const std::wstring flag = log::path(L".dump");
        if (GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(flag.c_str());
            why = "flag";
        }
    }
    if (why) take(static_cast<const uint8_t *>(world), calls, why);
}

}  // namespace crsrt::snapshot
