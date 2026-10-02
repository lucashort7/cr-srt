// dict_test.cpp — the dictionary against files in tests\dict-test\: what a first write makes, what a later write keeps
// of a file edited by hand, a key queued by another thread, and the types read off a registry laid out in memory as
// the game lays it out.
#include "dict.h"
#include "check.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace crsrt;

namespace {

constexpr const wchar_t *kFolder = L".\\dict-test\\";
constexpr const wchar_t *kFiles[] = {L"types.tsv", L"kinds.tsv", L"factions.tsv"};
constexpr uint8_t kBlobBytes[8] = {0xca, 0x48, 0x91, 0xc2, 0x9b, 0xb3, 0x73, 0xc5};  // blob, as the log prints it
constexpr uint8_t kBruteBytes[8] = {0xfb, 0x0c, 0x53, 0x9b, 0x1d, 0xf3, 0xd2, 0x06};
constexpr uint32_t kHiss = 0x4aac31a8;
constexpr const char *kKindsHeader = "key\tname\tmax\tlevel\tfaction\tb3c\tshield\tfirst_seen";

uint64_t key_of(const uint8_t (&bytes)[8]) {
    uint64_t key = 0;
    std::memcpy(&key, bytes, sizeof key);
    return key;
}

std::wstring at(const wchar_t *file) { return std::wstring(kFolder) + file; }

void wipe() {
    for (const wchar_t *f : kFiles) {
        DeleteFileW(at(f).c_str());
        DeleteFileW((at(f) + L".tmp").c_str());
    }
    RemoveDirectoryW(L".\\dict-test");
}

void put(const wchar_t *file, const std::string &text) {
    CreateDirectoryW(L".\\dict-test", nullptr);
    HANDLE h = CreateFileW(at(file).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD n = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(h);
}

// The file's bytes, "" when it is missing.
std::string slurp(const wchar_t *file) {
    HANDLE h = CreateFileW(at(file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[1 << 16];
    DWORD n = 0;
    ReadFile(h, buf, sizeof buf, &n, nullptr);
    CloseHandle(h);
    return {buf, n};
}

using Rows = std::vector<std::vector<std::string>>;

// Lines split on tabs, the line ends dropped.
Rows rows(const std::string &text) {
    Rows out;
    size_t a = 0;
    while (a < text.size()) {
        size_t e = text.find('\n', a);
        if (e == std::string::npos) e = text.size();
        std::string line(text, a, e - a);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> cells;
        size_t c = 0;
        for (;;) {
            const size_t t = line.find('\t', c);
            cells.emplace_back(line, c, t == std::string::npos ? std::string::npos : t - c);
            if (t == std::string::npos) break;
            c = t + 1;
        }
        out.push_back(cells);
        a = e + 1;
    }
    return out;
}

// Cell c of line r, "" past the file's end or the line's.
std::string cell(const Rows &t, size_t r, size_t c) { return r < t.size() && c < t[r].size() ? t[r][c] : ""; }

size_t width(const Rows &t, size_t r) { return r < t.size() ? t[r].size() : 0; }

// Line r's cells, none past the file's end.
std::vector<std::string> line(const Rows &t, size_t r) { return r < t.size() ? t[r] : std::vector<std::string>(); }

dict::Stats blob_stats() { return {3648, 8, kHiss, 2, 640}; }

}  // namespace

TEST(a_first_write_makes_each_file_with_its_header) {
    wipe();
    dict::open_at(kFolder);
    dict::kind(key_of(kBlobBytes), blob_stats());
    dict::faction(kHiss);
    dict::faction(0x00ab12cd);
    dict::step(true);
    const Rows kinds = rows(slurp(L"kinds.tsv"));
    CHECK(kinds.size() == 2);
    CHECK(line(kinds, 0) == rows(kKindsHeader)[0]);
    CHECK(width(kinds, 1) == 8);
    CHECK(cell(kinds, 1, 0) == "ca4891c29bb373c5");
    CHECK(cell(kinds, 1, 1).empty());  // a kind's name is ours
    CHECK(cell(kinds, 1, 2) == "3648");
    CHECK(cell(kinds, 1, 3) == "8");
    CHECK(cell(kinds, 1, 4) == "4aac31a8");
    CHECK(cell(kinds, 1, 5) == "2");
    CHECK(cell(kinds, 1, 6) == "640");
    CHECK(cell(kinds, 1, 7).size() == 19);  // "2026-10-01 21:56:55"
    const Rows factions = rows(slurp(L"factions.tsv"));
    CHECK(factions.size() == 3);
    CHECK(line(factions, 0) == (std::vector<std::string>{"hash", "name", "first_seen"}));
    CHECK(cell(factions, 1, 0) == "4aac31a8");
    CHECK(cell(factions, 2, 0) == "00ab12cd");
    CHECK(slurp(L"types.tsv").empty());  // nothing to say, no file
}

TEST(a_fill_keeps_a_name_typed_by_hand_a_column_of_ours_and_a_comment) {
    wipe();
    put(L"kinds.tsv", std::string(kKindsHeader) +
                          "\tnotes\n# met in the Pattern\nca4891c29bb373c5\tblob\t\t\t\t\t\t\tthe first one\n");
    dict::open_at(kFolder);
    CHECK(dict::known(dict::kKinds, key_of(kBlobBytes)));
    dict::kind(key_of(kBlobBytes), blob_stats());
    dict::step(true);
    const Rows kinds = rows(slurp(L"kinds.tsv"));
    CHECK(kinds.size() == 3);
    CHECK(cell(kinds, 0, 8) == "notes");
    CHECK(width(kinds, 1) == 1);
    CHECK(cell(kinds, 1, 0) == "# met in the Pattern");
    CHECK(width(kinds, 2) == 9);
    CHECK(cell(kinds, 2, 1) == "blob");
    CHECK(cell(kinds, 2, 2) == "3648");
    CHECK(cell(kinds, 2, 6) == "640");
    CHECK(cell(kinds, 2, 7).empty());  // the row was there: no first_seen of ours
    CHECK(cell(kinds, 2, 8) == "the first one");
}

TEST(a_complete_row_is_never_written_again) {
    wipe();
    const std::string text = std::string(kKindsHeader) + "\nca4891c29bb373c5\tblob\t3648\t8\t4aac31a8\t2\t640\t\n";
    put(L"kinds.tsv", text);
    dict::open_at(kFolder);
    dict::kind(key_of(kBlobBytes), {9999, 11, kHiss, 4, 0});
    dict::step(true);
    CHECK(slurp(L"kinds.tsv") == text);
}

DWORD WINAPI offer_brute(LPVOID) {
    dict::offer(dict::kKinds, key_of(kBruteBytes));
    return 0;
}

TEST(a_kind_queued_by_a_game_thread_lands_bare_and_the_walk_fills_it) {
    wipe();
    dict::open_at(kFolder);
    HANDLE t = CreateThread(nullptr, 0, offer_brute, nullptr, 0, nullptr);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    CHECK(!dict::known(dict::kKinds, key_of(kBruteBytes)));  // queued, not yet taken
    dict::step(true);
    CHECK(dict::known(dict::kKinds, key_of(kBruteBytes)));
    Rows kinds = rows(slurp(L"kinds.tsv"));
    CHECK(kinds.size() == 2);
    CHECK(width(kinds, 1) == 8);
    CHECK(cell(kinds, 1, 0) == "fb0c539b1df3d206");
    CHECK(cell(kinds, 1, 2).empty());
    CHECK(cell(kinds, 1, 7).size() == 19);
    dict::kind(key_of(kBruteBytes), {5237, 7, kHiss, 2, 0});
    dict::step(true);
    kinds = rows(slurp(L"kinds.tsv"));
    CHECK(kinds.size() == 2);
    CHECK(cell(kinds, 1, 2) == "5237");
    CHECK(cell(kinds, 1, 6) == "0");
}

TEST(an_edit_between_two_writes_is_kept) {
    wipe();
    dict::open_at(kFolder);
    dict::kind(key_of(kBlobBytes), blob_stats());
    dict::step(true);
    Rows kinds = rows(slurp(L"kinds.tsv"));
    CHECK(kinds.size() == 2);
    put(L"kinds.tsv",
        std::string(kKindsHeader) + "\nca4891c29bb373c5\tblob\t3648\t8\t4aac31a8\t2\t640\t" + cell(kinds, 1, 7) + "\n");
    dict::kind(key_of(kBruteBytes), {5237, 7, kHiss, 2, 0});
    dict::step(true);
    kinds = rows(slurp(L"kinds.tsv"));
    CHECK(kinds.size() == 3);
    CHECK(cell(kinds, 1, 1) == "blob");
    CHECK(cell(kinds, 2, 0) == "fb0c539b1df3d206");
}

TEST(a_key_typed_in_capitals_or_with_0x_is_the_same_key) {
    wipe();
    put(L"kinds.tsv", std::string(kKindsHeader) + "\nCA4891C29BB373C5\tblob\n");
    put(L"factions.tsv", "hash\tname\tfirst_seen\n0x4AAC31A8\thiss\t\n");
    dict::open_at(kFolder);
    CHECK(dict::known(dict::kKinds, key_of(kBlobBytes)));
    CHECK(dict::known(dict::kFactions, kHiss));
    dict::kind(key_of(kBlobBytes), blob_stats());
    dict::step(true);
    const Rows kinds = rows(slurp(L"kinds.tsv"));
    CHECK(kinds.size() == 2);
    CHECK(width(kinds, 1) == 8);  // a short row that changes is written as wide as the header
    CHECK(cell(kinds, 1, 0) == "CA4891C29BB373C5");
    CHECK(cell(kinds, 1, 2) == "3648");
}

TEST(a_file_without_its_key_column_is_left_alone) {
    wipe();
    const std::string text = "name\tmax\nblob\t3648\n";
    put(L"kinds.tsv", text);
    dict::open_at(kFolder);
    dict::kind(key_of(kBlobBytes), blob_stats());
    dict::step(true);
    CHECK(slurp(L"kinds.tsv") == text);
}

TEST(a_bom_and_crlf_are_kept) {
    wipe();
    put(L"factions.tsv", "\xEF\xBB\xBFhash\tname\tfirst_seen\r\n4aac31a8\thiss\t\r\n");
    dict::open_at(kFolder);
    dict::faction(0x1d82b4ed);
    dict::step(true);
    const std::string text = slurp(L"factions.tsv");
    CHECK(text.compare(0, 3, "\xEF\xBB\xBF") == 0);
    CHECK(text.find("\r\n4aac31a8\thiss\t\r\n1d82b4ed\t\t") != std::string::npos);
    CHECK(text.size() >= 2);
    CHECK(text.compare(text.size() - 2, 2, "\r\n") == 0);
}

// The registry as the runner's world holds it (game.h): world + 0 -> {hashes, count, records}, the hashes sorted, a
// record's name a string_view into a longer string. FactionData's and Health's names hash to their keys; the first
// does not and is kept nameless.
struct FakeRegistry {
    uint32_t hashes[3] = {0x12345678, 0x3796cba6, 0x3dc2bc5c};
    alignas(8) uint8_t records[3 * 0x58] = {};
    alignas(8) uint8_t registry[0x18] = {};
    alignas(8) uint8_t world[0x20] = {};

    FakeRegistry() {
        static const char health[] = "r::getTypeName<struct heron::health::component::Health>(void)";
        static const char faction[] = "heron::faction::component::FactionData";
        static const char other[] = "nothing hashes to this";
        set(0, other, sizeof other - 1, 8);
        set(1, faction, sizeof faction - 1, 4);
        set(2, health + 22, 32, 0x40);
        const auto hashes_at = reinterpret_cast<uint64_t>(hashes), records_at = reinterpret_cast<uint64_t>(records);
        const uint32_t count = 3;
        std::memcpy(registry, &hashes_at, 8);
        std::memcpy(registry + 8, &count, 4);
        std::memcpy(registry + 0x10, &records_at, 8);
        const auto registry_at = reinterpret_cast<uint64_t>(registry);
        std::memcpy(world, &registry_at, 8);
    }
    void set(int i, const char *name, uint64_t length, uint32_t size) {
        const auto p = reinterpret_cast<uint64_t>(name);
        std::memcpy(records + i * 0x58, &p, 8);
        std::memcpy(records + i * 0x58 + 8, &length, 8);
        std::memcpy(records + i * 0x58 + 0x10, &size, 4);
    }
    uint64_t at() const { return reinterpret_cast<uint64_t>(world); }
};

TEST(types_come_from_the_registry_with_the_names_that_hash_to_their_keys) {
    wipe();
    static FakeRegistry fake;
    dict::open_at(kFolder);
    CHECK(dict::types(mem::live(), fake.at()));
    dict::step(true);
    const Rows types = rows(slurp(L"types.tsv"));
    CHECK(types.size() == 4);
    CHECK(line(types, 0) == (std::vector<std::string>{"hash", "name", "size", "first_seen"}));
    CHECK(cell(types, 1, 0) == "12345678");
    CHECK(cell(types, 1, 1).empty());
    CHECK(cell(types, 1, 2) == "0x8");
    CHECK(cell(types, 2, 0) == "3796cba6");
    CHECK(cell(types, 2, 1) == "heron::faction::component::FactionData");
    CHECK(cell(types, 2, 2) == "0x4");
    CHECK(cell(types, 3, 0) == "3dc2bc5c");
    CHECK(cell(types, 3, 1) == "heron::health::component::Health");
    CHECK(cell(types, 3, 2) == "0x40");
    CHECK(dict::known(dict::kTypes, 0x3dc2bc5c));

    // a second session: the complete rows load, the nameless one is read again and stays nameless, nothing is written
    const std::string before = slurp(L"types.tsv");
    dict::open_at(kFolder);
    CHECK(dict::types(mem::live(), fake.at()));
    dict::step(true);
    CHECK(slurp(L"types.tsv") == before);
}

TEST(a_registry_whose_hashes_are_not_sorted_is_not_one) {
    wipe();
    static FakeRegistry fake;
    std::swap(fake.hashes[0], fake.hashes[2]);
    dict::open_at(kFolder);
    CHECK(!dict::types(mem::live(), fake.at()));
    dict::step(true);
    CHECK(slurp(L"types.tsv").empty());
}

TEST(a_registry_that_does_not_read_is_tried_again) {
    wipe();
    alignas(8) static uint8_t world[0x20] = {};  // registry pointer 0
    dict::open_at(kFolder);
    CHECK(!dict::types(mem::live(), reinterpret_cast<uint64_t>(world)));
    CHECK(!dict::types(mem::live(), reinterpret_cast<uint64_t>(world)));
    dict::step(true);
    CHECK(slurp(L"types.tsv").empty());
    wipe();
}
