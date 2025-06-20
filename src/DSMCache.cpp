#include "DSMCache.h"

#include <glog/logging.h>
#include <inttypes.h>

#include "util/Pre.h"

DSMCache::DSMCache(const DSMCacheConfig &cache_config)
{
    DLOG_IF(WARNING, cache_config.cacheSize % 1_MB != 0)
        << "cache size " << cache_config.cacheSize << "is not aligned to MB.";
    size = cache_config.cacheSize;
    auto *alloc = hugePageAlloc(size);
    LOG_IF(FATAL, alloc == nullptr) << "Failed to alloc huge page " << size;
    data = (uint64_t) alloc;
}