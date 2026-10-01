// source.cpp
#include "source.h"

#include "memory.h"

namespace crsrt::mem {

namespace {

class Live final : public Source {
   public:
    bool copy(uint64_t at, void *out, size_t n) const override {
        return at && mem::copy(reinterpret_cast<const void *>(at), out, n);
    }
};

const Live g_live;

}  // namespace

const Source &live() { return g_live; }

}  // namespace crsrt::mem
