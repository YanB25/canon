#include "memory/dsm_allocator.h"

#include "memory/tag_allocator.h"
#include "util/CRTP.h"

namespace memory
{

DSMAllocator::DSMAllocator(DSM *dsm)
{
    using Provider = mem::TagAllocator<RemoteTag>::Provider;
    std::vector<Provider> providers;

    // the order is important: distribute across machine
    for (size_t d = 0; d < NR_DIRECTORY; ++d)
    {
        for (size_t m = 0; m < dsm->getClusterSize(); ++m)
        {
            auto chunk_alloc =
                std::make_shared<memory::RMChunkAllocator>(dsm, m, d);
            auto slab_alloc = std::make_shared<mem::LazySlabAllocator>(
                chunk_alloc, define::kChunkSize);
            providers.emplace_back(Provider{
                .tag = RemoteTag{.node_id = (uint8_t) m, .dir_id = (uint8_t) d},
                .allocator = slab_alloc});
        }
    }

    alloc_ = std::make_shared<mem::TagAllocator<RemoteTag>>(
        std::move(providers), dsm->get_thread_id());
}

}  // namespace memory