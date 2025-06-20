#pragma once
#include <type_traits>
#include <utility>

#include "./allocator.h"
#include "memory/refill_allocator.h"
#include "util/Likely.h"
#include "util/UP.h"
#include "util/concept.h"

namespace mem
{

template <Hashable Tag>
requires util::Ostreamable<Tag>
class TagAllocator
{
public:
    struct Provider
    {
        Tag tag;
        mem::BaseAllocator::pointer allocator;
    };
    using pointer = std::shared_ptr<TagAllocator>;

    TagAllocator(std::vector<Provider> &&providers, size_t start_idx)
        : providers_(std::move(providers)),
          rr_idx_(start_idx % providers_.size())
    {
        for (auto &provider : providers_)
        {
            // auto p = std::make_pair<Tag, Provider *>(provider.tag,
            // &provider);
            auto [it, ok] = tag_to_provider_.emplace(
                std::make_pair(provider.tag, &provider));
            CHECK(ok) << "** tag duplicated: tag " << provider.tag << ", "
                      << util::pre(tag_to_provider_);
        }
    }

    std::pair<Tag, void *> alloc(size_t size, size_t alignment)
    {
        auto &provider = providers_[rr_idx_++ % providers_.size()];
        auto addr = provider.allocator->alloc(size, alignment);
        DCHECK_EQ((uint64_t) addr % alignment, 0);
        return {provider.tag, addr};
    }

    void *alloc_from(size_t size, const Tag &tag, size_t alignment)
    {
        auto it = tag_to_provider_.find(tag);
        if (unlikely(it == tag_to_provider_.end()))
        {
            LOG(FATAL) << "** unknown tag " << tag << ": "
                       << util::pre(tag_to_provider_);
        }
        auto *ret = it->second->allocator->alloc(size, alignment);
        DCHECK_EQ((uint64_t) ret % alignment, 0);
        return ret;
    }
    void free_to(void *addr, size_t size, const Tag &tag)
    {
        if (likely(addr != nullptr))
        {
            auto it = tag_to_provider_.find(tag);
            if (unlikely(it == tag_to_provider_.end()))
            {
                LOG(FATAL) << "** unknown tag " << tag << ": "
                           << util::pre(tag_to_provider_);
            }
            return it->second->allocator->free(addr, size);
        }
    }
    template <typename U>
    friend std::ostream &operator<<(std::ostream &os, const TagAllocator<U> &a);

private:
    std::vector<Provider> providers_;
    std::unordered_map<Tag, Provider *> tag_to_provider_;
    size_t rr_idx_;
};

template <typename Tag>
inline std::ostream &operator<<(std::ostream &os, const TagAllocator<Tag> &a)
{
    os << "[Tag]" << std::endl;
    for (const auto &p : a.providers_)
    {
        os << "tag: " << p.tag << ": ";
        auto *lazy_allocator =
            dynamic_cast<mem::LazySlabAllocator *>(p.allocator.get());
        if (lazy_allocator)
        {
            os << *lazy_allocator << std::endl;
        }
        else
        {
            os << "unknown";
        }
    }
    os << "END [tag]";
    return os;
}

}  // namespace mem