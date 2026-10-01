// snapfile.cpp
#include "snapfile.h"

#include "game.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace crsrt::snapfile {

namespace {

// A cursor over the file that refuses to read past its end.
struct Reader {
    const std::vector<uint8_t> &bytes;
    size_t at = 0;
    bool ok = true;

    template <class T>
    T take() {
        T v{};
        if (!skip(sizeof v)) return v;
        std::memcpy(&v, bytes.data() + at - sizeof v, sizeof v);
        return v;
    }
    bool skip(size_t n) {
        if (!ok || bytes.size() - at < n) return ok = false;
        at += n;
        return true;
    }
};

uint64_t u64_at(const std::vector<uint8_t> &bytes, size_t offset) {
    uint64_t v = 0;
    std::memcpy(&v, bytes.data() + offset, sizeof v);
    return v;
}

}  // namespace

bool Snapshot::load(const char *path) {
    *this = {};
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bytes_.resize(size > 0 ? static_cast<size_t>(size) : 0);
    const bool read = !bytes_.empty() && std::fread(bytes_.data(), 1, bytes_.size(), f) == bytes_.size();
    std::fclose(f);
    if (!read || bytes_.size() < sizeof kMagic || std::memcmp(bytes_.data(), kMagic, sizeof kMagic) != 0) {
        *this = {};
        return false;
    }

    Reader r{bytes_, sizeof kMagic};
    world_ = r.take<uint64_t>();
    calls_ = r.take<uint32_t>();
    r.take<uint32_t>();  // slots scanned
    const uint32_t world_len = r.take<uint32_t>();
    blocks_.push_back({world_, r.at, world_len});
    r.skip(world_len);

    while (r.ok) {
        const uint32_t tag = r.take<uint32_t>();
        if (tag == kEndTag) break;
        if (tag != kArchTag) r.ok = false;
        const int32_t index = r.take<int32_t>();
        const uint64_t chunk = r.take<uint64_t>();
        const uint32_t count = r.take<uint32_t>();
        const uint32_t blob = r.take<uint32_t>();
        const size_t type_entry = r.at, offsets_entry = r.at + 32;
        if (!r.skip(64)) break;
        const size_t hashes = r.at, offsets = r.at + count * 4ull;
        if (!r.skip(count * 8ull)) break;
        const uint32_t tags = r.take<uint32_t>();
        const size_t tag_list = r.at;
        if (!r.skip(tags * 4ull)) break;
        const size_t chunk_bytes = r.at;
        if (!r.skip(blob)) break;

        const uint64_t w = world_ + 8;
        const auto slot = static_cast<uint64_t>(index);
        blocks_.push_back({w + (slot + game::kTypeEntryBias) * 32, type_entry, 32});
        blocks_.push_back({w + slot * 32 + game::kOffsetsBase, offsets_entry, 32});
        blocks_.push_back({u64_at(bytes_, type_entry + 8), hashes, count * 4ull});
        blocks_.push_back({u64_at(bytes_, offsets_entry), offsets, count * 4ull});
        if (tags) blocks_.push_back({u64_at(bytes_, type_entry), tag_list, tags * 4ull});
        if (blob) blocks_.push_back({chunk, chunk_bytes, blob});
        archetypes_.push_back({index, chunk, count, blob});
    }
    if (!r.ok) {
        *this = {};
        return false;
    }
    return true;
}

bool Snapshot::copy(uint64_t at, void *out, size_t n) const {
    const auto holds = [&](const Block &b) { return at >= b.at && at - b.at <= b.size && n <= b.size - (at - b.at); };
    const auto b = std::ranges::find_if(blocks_, holds);
    if (b == blocks_.end()) return false;
    std::memcpy(out, bytes_.data() + b->offset + (at - b->at), n);
    return true;
}

}  // namespace crsrt::snapfile
