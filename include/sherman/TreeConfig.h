#pragma once
#include <cinttypes>

#include "Common.h"
#include "GlobalAddress.h"
#include "avis/provider.h"

namespace avis
{
class AvisHandle;
}

namespace define
{
constexpr uint64_t kRootPointerStoreOffest = define::kChunkSize / 2;
static_assert(kRootPointerStoreOffest % sizeof(uint64_t) == 0, "XX");
constexpr uint8_t kMaxHandOverTime = 8;
// level of tree
constexpr uint64_t kMaxLevelOfTree = 7;
// number of locks
constexpr uint64_t kNumOfLock = define::kLockChipMemSize / sizeof(uint64_t);

constexpr int kIndexCacheSize = 1024;  // MB
}  // namespace define

#define LATENCY_WINDOWS 1000000

struct TreeConfig
{
    bool is_local{false};
    size_t bucket_nr = 1024;
    size_t cache_limit = std::numeric_limits<size_t>::max();

    size_t allocate_batch_size_{define::kChunkSize};

    struct Avis
    {
        std::vector<avis::BuddyProvider> providers;
        size_t server_nid;
        GlobalAddress pub_meta;
        size_t pub_size;
    };
    std::optional<Avis> avis_;
};