// dict.cpp
#include "dict.h"

#include "clock.h"
#include "game.h"
#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace crsrt::dict {

namespace {

constexpr int kSlotBits = 13;
constexpr uint32_t kSlots = 1u << kSlotBits;
constexpr uint32_t kMaxKeys = kSlots / 4 * 3;  // a quarter of the slots stays empty, so every probe ends
constexpr int kMaxOffers = 1024;
constexpr int64_t kFlushGapUs = 2'000'000;
constexpr int64_t kTypesRetryUs = 10'000'000;  // a world whose registry did not read, tried again after this
constexpr uint32_t kMaxTypes = 0x4000;         // a registry count past this is not one
constexpr uint32_t kMaxName = 2048;
constexpr uint64_t kMaxFile = 64ull << 20;
constexpr int kMaxColumns = 8;

// The columns of each file, by index in its schema.
enum : uint8_t { kTypeHash, kTypeNameColumn, kTypeSizeColumn, kTypeFirst };
enum : uint8_t { kKindKey, kKindName, kKindMax, kKindLevel, kKindFaction, kKindBossType, kKindShield, kKindFirst };
enum : uint8_t { kFactionHash, kFactionName, kFactionFirst };

constexpr uint8_t bit(int column) { return static_cast<uint8_t>(1u << column); }

// A class's file: its columns in order, the key first and first_seen last. `complete` is the cells the DLL fills
// while they are empty; first_seen is written only with the row.
struct Schema {
    const wchar_t *file;
    const char *noun;  // for the log
    const char *columns[kMaxColumns];
    int count;
    uint8_t complete;
    bool bytes;  // the key printed as its 8 bytes in memory order (a kind), else as a 32-bit hash
};

constexpr uint8_t kTypeCells = bit(kTypeNameColumn) | bit(kTypeSizeColumn);
constexpr uint8_t kKindStats =
    bit(kKindMax) | bit(kKindLevel) | bit(kKindFaction) | bit(kKindBossType) | bit(kKindShield);
constexpr Schema kSchemas[kClasses] = {
    {L"types.tsv", "type", {"hash", "name", "size", "first_seen"}, 4, kTypeCells, false},
    {L"kinds.tsv",
     "kind",
     {"key", "name", "max", "level", "faction", "boss_type", "shield", "first_seen"},
     8,
     kKindStats,
     true},
    {L"factions.tsv", "faction", {"hash", "name", "first_seen"}, 3, 0, false},
};

// A row to add, or the cells to fill in a row already there: applied to the file as it is on disk when it is written.
struct Change {
    uint64_t key = 0;
    std::string cells[kMaxColumns];  // "" leaves the cell as it is
};

// One class: its keys, an open-addressed set that only the walk thread inserts into and that is never shrunk, so a
// reader on another thread sees a key whole or not at all; then what only the walk thread touches.
struct Book {
    std::atomic<uint64_t> keys[kSlots];  // 0 = an empty slot
    uint8_t filled[kSlots];              // bit i: the row's column i holds something
    uint32_t count;
    std::vector<Change> pending;
    std::wstring path;
    bool refused;  // the file has no key column: nothing is loaded or written
    bool full_said, write_said;
};
Book g_books[kClasses];

struct Offer {
    Class cls;
    uint64_t key;
};
SRWLOCK g_offer_lock = SRWLOCK_INIT;
Offer g_offers[kMaxOffers];  // under g_offer_lock
int g_offer_count;           // under g_offer_lock
long g_offers_dropped;       // under g_offer_lock
Offer g_taken[kMaxOffers];   // the walk thread's copy

bool g_opened;
int64_t g_flushed_at = -kFlushGapUs;
uint64_t g_types_world, g_types_failed_world;
int64_t g_types_failed_at;
bool g_types_said;
char g_name[kMaxName];

uint32_t home(uint64_t key) { return static_cast<uint32_t>((key * 0x9e3779b97f4a7c15ull) >> (64 - kSlotBits)); }
uint32_t next(uint32_t slot) { return (slot + 1) & (kSlots - 1); }

bool contains(const Book &b, uint64_t key) {
    for (uint32_t i = home(key), n = 0; n < kSlots; i = next(i), n++) {
        const uint64_t k = b.keys[i].load(std::memory_order_acquire);
        if (k == key) return true;
        if (!k) return false;
    }
    return false;
}

// The walk thread's: key's slot, or -1.
int32_t find(const Book &b, uint64_t key) {
    for (uint32_t i = home(key), n = 0; n < kSlots; i = next(i), n++) {
        const uint64_t k = b.keys[i].load(std::memory_order_relaxed);
        if (k == key) return static_cast<int32_t>(i);
        if (!k) return -1;
    }
    return -1;
}

// The walk thread's: the slot key goes into, absent before; -1 when the book is full.
int32_t insert(Book &b, uint64_t key) {
    if (b.count >= kMaxKeys) return -1;
    uint32_t i = home(key);
    while (b.keys[i].load(std::memory_order_relaxed)) i = next(i);
    b.filled[i] = 0;
    b.keys[i].store(key, std::memory_order_release);
    b.count++;
    return static_cast<int32_t>(i);
}

uint32_t fnv1a32(const char *s, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<uint8_t>(s[i])) * 0x01000193u;
    return h;
}

// "2026-10-01 21:56:55", the log's own stamp.
std::string now_text() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char out[32];
    wsprintfA(out, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return out;
}

std::string key_text(const Schema &s, uint64_t key) {
    char out[24];
    if (s.bytes) {
        uint8_t b[8];
        std::memcpy(b, &key, sizeof b);
        wsprintfA(out, "%02x%02x%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    } else {
        wsprintfA(out, "%08lx", static_cast<unsigned long>(key));
    }
    return out;
}

std::string number(long v) {
    char out[16];
    wsprintfA(out, "%ld", v);
    return out;
}

std::string trimmed(const std::string &s) {
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The key a cell holds, hex with or without 0x: 16 digits for a kind, its bytes in memory order; 1 to 8 for a hash.
// 0 when it holds none.
uint64_t parse_key(const Schema &s, const std::string &cell) {
    std::string t = trimmed(cell);
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t.erase(0, 2);
    if (t.empty() || t.size() > (s.bytes ? 16u : 8u) || (s.bytes && t.size() != 16)) return 0;
    for (const char c : t)
        if (hex_digit(c) < 0) return 0;
    uint64_t key = 0;
    if (s.bytes) {
        uint8_t b[8];
        for (int i = 0; i < 8; i++) b[i] = static_cast<uint8_t>(hex_digit(t[i * 2]) << 4 | hex_digit(t[i * 2 + 1]));
        std::memcpy(&key, b, sizeof key);
    } else {
        for (const char c : t) key = key << 4 | static_cast<uint64_t>(hex_digit(c));
    }
    return key;
}

std::vector<std::string> split(const std::string &line) {
    std::vector<std::string> cells;
    size_t at = 0;
    for (;;) {
        const size_t tab = line.find('\t', at);
        cells.emplace_back(line, at, tab == std::string::npos ? std::string::npos : tab - at);
        if (tab == std::string::npos) return cells;
        at = tab + 1;
    }
}

std::string joined(const std::vector<std::string> &cells) {
    std::string line;
    for (size_t i = 0; i < cells.size(); i++) {
        if (i) line += '\t';
        line += cells[i];
    }
    return line;
}

// A file as lines, without their ends; the first line's end is the one written back, and a UTF-8 BOM is kept aside.
struct Text {
    std::vector<std::string> lines;
    std::string eol = "\n";
    bool bom = false;
};

// The file's lines; none when it is missing. False when it is there and does not read.
bool read_text(const std::wstring &path, Text &out) {
    out = {};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    LARGE_INTEGER size{};
    std::string data;
    bool ok = GetFileSizeEx(h, &size) && static_cast<uint64_t>(size.QuadPart) <= kMaxFile;
    if (ok) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = data.empty() ||
             (ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) && got == data.size());
    }
    CloseHandle(h);
    if (!ok) return false;
    if (data.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        out.bom = true;
        data.erase(0, 3);
    }
    size_t at = 0;
    while (at < data.size()) {
        size_t end = data.find('\n', at);
        if (end == std::string::npos) end = data.size();
        size_t stop = end;
        if (stop > at && data[stop - 1] == '\r') {
            stop--;
            if (out.lines.empty()) out.eol = "\r\n";
        }
        out.lines.emplace_back(data, at, stop - at);
        at = end + 1;
    }
    return true;
}

// Through a file beside it, on the disk before it is renamed over the original: a reader never sees half of it, and a
// crash of the machine never leaves an empty file where names were typed. A folder removed since open() is made again.
bool write_text(const std::wstring &path, const std::string &text) {
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND) {
        CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
        h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(tmp.c_str());
    return false;
}

// Each schema column's index in a header, -1 where the header lacks it.
void columns_of(const Schema &s, const std::vector<std::string> &header, int (&at)[kMaxColumns]) {
    for (int j = 0; j < s.count; j++) {
        at[j] = -1;
        for (size_t i = 0; i < header.size() && at[j] < 0; i++)
            if (trimmed(header[i]) == s.columns[j]) at[j] = static_cast<int>(i);
    }
}

std::string cell(const std::vector<std::string> &cells, int at) {
    return at >= 0 && static_cast<size_t>(at) < cells.size() ? trimmed(cells[at]) : std::string();
}

void load(Class c) {
    Book &b = g_books[c];
    const Schema &s = kSchemas[c];
    Text text;
    if (!read_text(b.path, text)) {
        log::write("dict: %ls does not read - starts empty, written when it reads", b.path.c_str());
        return;
    }
    if (text.lines.empty()) return;  // born with its header on the first write
    int at[kMaxColumns];
    columns_of(s, split(text.lines[0]), at);
    if (at[0] < 0) {
        b.refused = true;
        log::write("dict: %ls has no %s column - not loaded and never written", b.path.c_str(), s.columns[0]);
        return;
    }
    for (size_t i = 1; i < text.lines.size(); i++) {
        const std::vector<std::string> cells = split(text.lines[i]);
        const uint64_t key = parse_key(s, cell(cells, at[0]));
        if (!key || find(b, key) >= 0) continue;  // a line without a key, or a key repeated: the first row counts
        const int32_t slot = insert(b, key);
        if (slot < 0) {
            log::write("dict: %ls holds more than %lu keys - the rest not loaded", b.path.c_str(),
                       static_cast<unsigned long>(kMaxKeys));
            b.full_said = true;
            return;
        }
        for (int j = 1; j < s.count; j++)
            if (!cell(cells, at[j]).empty()) b.filled[slot] |= bit(j);
    }
}

// The cells of ch the row lacks, recorded for the next write; a key new to the book gets its row and first_seen.
// True when anything was recorded.
bool record(Class c, uint64_t key, Change &ch) {
    Book &b = g_books[c];
    const Schema &s = kSchemas[c];
    int32_t slot = find(b, key);
    const bool fresh = slot < 0;
    if (fresh) {
        slot = insert(b, key);
        if (slot < 0) {
            if (!b.full_said)
                log::write("dict: %ls is full at %lu keys", b.path.c_str(), static_cast<unsigned long>(kMaxKeys));
            b.full_said = true;
            return false;
        }
        ch.cells[s.count - 1] = now_text();
    }
    uint8_t adds = 0;
    for (int j = 1; j < s.count - 1; j++) {
        if (ch.cells[j].empty()) continue;
        if (b.filled[slot] & bit(j))
            ch.cells[j].clear();
        else
            adds |= bit(j);
    }
    if (!fresh && !adds) return false;
    b.filled[slot] |= adds;
    ch.key = key;
    b.pending.push_back(std::move(ch));
    return true;
}

// The book's pending changes onto the file as it is now: rows added at the end, empty cells filled, every other line
// written as it was read. A column the header lacks is added to it. False, the changes kept, when the file does not
// read, has no key column or does not write.
bool flush(Class c) {
    Book &b = g_books[c];
    const Schema &s = kSchemas[c];
    const int64_t t0 = clock::now_us();
    Text text;
    if (!read_text(b.path, text)) {
        if (!b.write_said) log::write("dict: %ls does not read - its changes wait", b.path.c_str());
        b.write_said = true;
        return false;
    }
    std::vector<std::string> header =
        text.lines.empty() ? std::vector<std::string>(s.columns, s.columns + s.count) : split(text.lines[0]);
    bool header_changed = text.lines.empty();
    if (text.lines.empty()) text.lines.emplace_back();
    int at[kMaxColumns];
    columns_of(s, header, at);
    if (at[0] < 0) {
        if (!b.write_said) log::write("dict: %ls has no %s column - its changes wait", b.path.c_str(), s.columns[0]);
        b.write_said = true;
        return false;
    }
    for (int j = 1; j < s.count; j++)
        if (at[j] < 0) {
            header.emplace_back(s.columns[j]);
            at[j] = static_cast<int>(header.size()) - 1;
            header_changed = true;
        }

    std::vector<std::vector<std::string>> cells(text.lines.size());
    std::vector<bool> changed(text.lines.size());
    std::unordered_map<uint64_t, size_t> rows;
    for (size_t i = 1; i < text.lines.size(); i++) {
        cells[i] = split(text.lines[i]);
        const uint64_t key = parse_key(s, cell(cells[i], at[0]));
        if (key) rows.emplace(key, i);  // a repeated key: the first row
    }
    int added = 0, filled = 0;
    for (const Change &ch : b.pending) {
        const auto it = rows.find(ch.key);
        if (it == rows.end()) {
            std::vector<std::string> row(header.size());
            row[at[0]] = key_text(s, ch.key);
            for (int j = 1; j < s.count; j++) row[at[j]] = ch.cells[j];
            rows.emplace(ch.key, text.lines.size());
            text.lines.emplace_back();
            cells.push_back(std::move(row));
            changed.push_back(true);
            added++;
            continue;
        }
        // a row shorter than the header is written back as long as it, if it changes
        std::vector<std::string> &row = cells[it->second];
        if (row.size() < header.size()) row.resize(header.size());
        bool any = false;
        for (int j = 1; j < s.count; j++) {
            if (ch.cells[j].empty() || !cell(row, at[j]).empty()) continue;
            row[at[j]] = ch.cells[j];
            any = true;
        }
        if (any) {
            changed[it->second] = true;
            filled++;
        }
    }

    std::string out = text.bom ? "\xEF\xBB\xBF" : "";
    out += header_changed ? joined(header) : text.lines[0];
    out += text.eol;
    for (size_t i = 1; i < text.lines.size(); i++) {
        out += changed[i] ? joined(cells[i]) : text.lines[i];
        out += text.eol;
    }
    if (!write_text(b.path, out)) {
        if (!b.write_said)
            log::write("dict: %ls does not write (error %lu; open in a program that locks it?) - its changes wait",
                       b.path.c_str(), GetLastError());
        b.write_said = true;
        return false;
    }
    b.write_said = false;
    b.pending.clear();
    log::write("dict: %ls written, %d rows added, %d filled, %lu lines (%ld us)", b.path.c_str(), added, filled,
               static_cast<unsigned long>(text.lines.size()), static_cast<long>(clock::now_us() - t0));
    return true;
}

// What the game threads queued since the last drain.
void drain() {
    AcquireSRWLockExclusive(&g_offer_lock);
    const int n = g_offer_count;
    std::memcpy(g_taken, g_offers, sizeof(Offer) * n);
    g_offer_count = 0;
    const long dropped = g_offers_dropped;
    g_offers_dropped = 0;
    ReleaseSRWLockExclusive(&g_offer_lock);
    if (dropped) log::write("dict: %ld keys dropped, the queue was full", dropped);
    for (int i = 0; i < n; i++) {
        const Offer &o = g_taken[i];
        if (g_books[o.cls].refused || find(g_books[o.cls], o.key) >= 0) continue;
        Change ch;
        if (!record(o.cls, o.key, ch)) continue;
        const Schema &s = kSchemas[o.cls];
        log::write("dict: new %s %s, queued by a game thread%s", s.noun, key_text(s, o.key).c_str(),
                   o.cls == kKinds ? " (the spawn queue): its stats when the walk meets it" : "");
    }
}

void reset() {
    for (Book &b : g_books) {
        for (auto &k : b.keys) k.store(0, std::memory_order_relaxed);
        std::memset(b.filled, 0, sizeof b.filled);
        b.count = 0;
        b.pending.clear();
        b.path.clear();
        b.refused = b.full_said = b.write_said = false;
    }
    AcquireSRWLockExclusive(&g_offer_lock);
    g_offer_count = 0;
    g_offers_dropped = 0;
    ReleaseSRWLockExclusive(&g_offer_lock);
    g_opened = false;
    g_flushed_at = -kFlushGapUs;
    g_types_world = g_types_failed_world = 0;
    g_types_failed_at = 0;
    g_types_said = false;
}

void open_folder(std::wstring folder) {
    g_opened = true;
    if (folder.empty() || folder.back() != L'\\') folder += L'\\';
    const std::wstring bare = folder.substr(0, folder.size() - 1);
    if (!CreateDirectoryW(bare.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        log::write("dict: %ls cannot be made (error %lu) - nothing kept", bare.c_str(), GetLastError());
        for (Book &b : g_books) b.refused = true;
        return;
    }
    for (int c = 0; c < kClasses; c++) {
        g_books[c].path = folder + kSchemas[c].file;
        load(static_cast<Class>(c));
    }
    log::write("dict: %ls holds %lu types, %lu kinds, %lu factions", folder.c_str(),
               static_cast<unsigned long>(g_books[kTypes].count), static_cast<unsigned long>(g_books[kKinds].count),
               static_cast<unsigned long>(g_books[kFactions].count));
}

}  // namespace

bool known(Class c, uint64_t key) { return key && c < kClasses && contains(g_books[c], key); }

void offer(Class c, uint64_t key) {
    if (!key || c >= kClasses || contains(g_books[c], key)) return;
    AcquireSRWLockExclusive(&g_offer_lock);
    if (g_offer_count < kMaxOffers)
        g_offers[g_offer_count++] = {c, key};
    else
        g_offers_dropped++;
    ReleaseSRWLockExclusive(&g_offer_lock);
}

void open() {
    if (!g_opened) open_folder(log::beside(L"dict"));
}

void open_at(const wchar_t *folder) {
    reset();
    open_folder(folder);
}

bool types(const mem::Source &source, uint64_t world) {
    if (!world || world == g_types_world) return world != 0;
    const int64_t t0 = clock::now_us();
    if (g_books[kTypes].refused || (world == g_types_failed_world && t0 - g_types_failed_at < kTypesRetryUs))
        return false;
    uint64_t registry = 0, hashes_at = 0, records_at = 0;
    uint32_t count = 0;
    bool ok = mem::read(source, world + game::kWorldTypeRegistry, registry) && registry &&
              mem::read(source, registry + game::kRegistryHashes, hashes_at) &&
              mem::read(source, registry + game::kRegistryCount, count) &&
              mem::read(source, registry + game::kRegistryRecords, records_at) && hashes_at && records_at && count &&
              count <= kMaxTypes;
    std::vector<uint32_t> hashes;
    std::vector<uint8_t> records;
    if (ok) {
        hashes.resize(count);
        records.resize(static_cast<size_t>(count) * game::kTypeRecordSize);
        ok = source.copy(hashes_at, hashes.data(), hashes.size() * sizeof(uint32_t)) &&
             source.copy(records_at, records.data(), records.size());
        // the game binary-searches them: a pointer that reads but is not the registry fails here, before any row
        for (uint32_t i = 1; ok && i < count; i++) ok = hashes[i - 1] < hashes[i];
    }
    if (!ok) {
        if (world != g_types_failed_world)
            log::write(
                "dict: the type registry of world %p does not read, or its hashes are not sorted (registry %p, %lu "
                "types) - tried again in 10 s",
                reinterpret_cast<void *>(world), reinterpret_cast<void *>(registry), static_cast<unsigned long>(count));
        g_types_failed_world = world;
        g_types_failed_at = t0;
        return false;
    }
    // Health's record naming Health vouches for the layout, and then a name that does not hash to its key is still the
    // key's, the registry pairing them by index as the game's own search does: the content components hash another way
    // (contentgame::ShieldComponent is 031b0c21, 447 of 1587 in build 1.4.0, 2026-10-01). Without it only a name that
    // hashes is kept.
    bool vouched = false;
    for (uint32_t i = 0; i < count && !vouched; i++) {
        if (hashes[i] != game::kHealth) continue;
        uint64_t name_at = 0, length = 0;
        std::memcpy(&name_at, records.data() + static_cast<size_t>(i) * game::kTypeRecordSize + game::kTypeName, 8);
        std::memcpy(&length, records.data() + static_cast<size_t>(i) * game::kTypeRecordSize + game::kTypeNameLength,
                    8);
        vouched = length && length <= kMaxName && source.copy(name_at, g_name, length) &&
                  fnv1a32(g_name, length) == game::kHealth;
    }
    Book &b = g_books[kTypes];
    int fresh = 0, filled = 0, unhashed = 0, unnamed = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t hash = hashes[i];
        const int32_t slot = hash ? find(b, hash) : -1;
        if (!hash || (slot >= 0 && (b.filled[slot] & kTypeCells) == kTypeCells)) continue;
        const uint8_t *rec = records.data() + static_cast<size_t>(i) * game::kTypeRecordSize;
        uint64_t name_at = 0, length = 0;
        uint32_t size = 0;
        std::memcpy(&name_at, rec + game::kTypeName, sizeof name_at);
        std::memcpy(&length, rec + game::kTypeNameLength, sizeof length);
        std::memcpy(&size, rec + game::kTypeSize, sizeof size);
        Change ch;
        const bool read = length && length <= kMaxName && source.copy(name_at, g_name, length);
        const bool hashed = read && fnv1a32(g_name, length) == hash;
        if (read && !hashed) unhashed++;
        if (hashed || (read && vouched)) {
            ch.cells[kTypeNameColumn].assign(g_name, length);
            for (char &k : ch.cells[kTypeNameColumn])
                if (k == '\t' || k == '\r' || k == '\n') k = ' ';
        } else {
            unnamed++;
        }
        char hex[16];
        wsprintfA(hex, "0x%lx", static_cast<unsigned long>(size));
        ch.cells[kTypeSizeColumn] = hex;
        if (record(kTypes, hash, ch)) (slot < 0 ? fresh : filled)++;
    }
    g_types_world = world;
    if (fresh || filled || !g_types_said)
        log::write(
            "dict: types: the registry of world %p holds %lu, %d new, %d filled; %d names read that do not hash to "
            "their key, kept: %s; %d left nameless (%ld us)",
            reinterpret_cast<void *>(world), static_cast<unsigned long>(count), fresh, filled, unhashed,
            vouched ? "yes, Health's record names Health" : "no", unnamed, static_cast<long>(clock::now_us() - t0));
    g_types_said = true;
    return true;
}

void kind(uint64_t key, const Stats &stats) {
    Book &b = g_books[kKinds];
    if (!key || b.refused) return;
    const int32_t slot = find(b, key);
    if (slot >= 0 && (b.filled[slot] & kKindStats) == kKindStats) return;
    Change ch;
    ch.cells[kKindMax] = number(stats.max);
    ch.cells[kKindLevel] = number(stats.level);
    char faction[16];
    wsprintfA(faction, "%08lx", static_cast<unsigned long>(stats.faction));
    ch.cells[kKindFaction] = faction;
    ch.cells[kKindBossType] = number(stats.boss_type);
    ch.cells[kKindShield] = number(stats.shield);
    if (!record(kKinds, key, ch)) return;
    log::write("dict: %s kind %s: max %ld lvl %ld faction %08lx boss_type %u shield %ld", slot < 0 ? "new" : "filled",
               key_text(kSchemas[kKinds], key).c_str(), static_cast<long>(stats.max), static_cast<long>(stats.level),
               static_cast<unsigned long>(stats.faction), stats.boss_type, static_cast<long>(stats.shield));
}

void faction(uint32_t hash) {
    Book &b = g_books[kFactions];
    if (!hash || b.refused || find(b, hash) >= 0) return;
    Change ch;
    if (record(kFactions, hash, ch)) log::write("dict: new faction %08lx", static_cast<unsigned long>(hash));
}

void step(bool force) {
    drain();
    const int64_t now = clock::now_us();
    if (!force && now - g_flushed_at < kFlushGapUs) return;
    bool any = false;
    for (int c = 0; c < kClasses; c++) {
        Book &b = g_books[c];
        if (b.pending.empty() || b.refused) continue;
        any = true;
        if (flush(static_cast<Class>(c)) || !force) continue;
        // the last write of the session failed: what it held goes to the log, which is all that keeps it now
        log::write("dict: %ls did not write before the end - %lu changes lost, their keys:", b.path.c_str(),
                   static_cast<unsigned long>(b.pending.size()));
        std::string keys;
        for (size_t i = 0; i < b.pending.size(); i++) {
            keys += ' ';
            keys += key_text(kSchemas[c], b.pending[i].key);
            if (keys.size() > 800 || i + 1 == b.pending.size()) {
                log::write("  %s", keys.c_str());
                keys.clear();
            }
        }
    }
    if (any) g_flushed_at = now;
}

}  // namespace crsrt::dict
