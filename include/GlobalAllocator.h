#pragma once

#include <glog/logging.h>

#include <cstring>

#include "Common.h"
#include "GlobalAddress.h"
#include "memory/allocator.h"
#include "util/Debug.h"

// global allocator for coarse-grained (chunck level) alloc
// used by home agent
// bitmap based
class GlobalAllocator
{
public:
    using pointer = std::shared_ptr<GlobalAllocator>;
    GlobalAllocator(const GlobalAddress &start, size_t size) : start(start)
    {
        bitmap_len = size / define::kChunkSize;
        bitmap = new bool[bitmap_len];
        memset(bitmap, 0, bitmap_len);

        // null ptr
        bitmap[0] = true;
        bitmap_tail = 1;
    }
    static std::shared_ptr<GlobalAllocator> newInstance(
        const GlobalAddress &start, size_t size)
    {
        return std::make_shared<GlobalAllocator>(start, size);
    }

    ~GlobalAllocator()
    {
        delete[] bitmap;
    }

    GlobalAddress alloc(size_t sz)
    {
        if (unlikely(sz == 0))
        {
            return GlobalAddress{};
        }

        auto chunk_nr = (sz + define::kChunkSize - 1) / define::kChunkSize;
        GlobalAddress ret = start;
        if (unlikely(bitmap_tail >= bitmap_len))
        {
            LOG(FATAL) << "shared memory space run out";
        }
        for (size_t i = 0; i < chunk_nr; ++i)
        {
            if (bitmap[bitmap_tail])
            {
                LOG(FATAL) << "TODO: can not satisify this allocation: "
                           << PRE(bitmap_tail);
            }
            else
            {
                bitmap[bitmap_tail] = true;
            }
        }
        ret.offset += bitmap_tail * define::kChunkSize;
        bitmap_tail += chunk_nr;
        return ret;
    }

    GlobalAddress alloc_chunck()
    {
        GlobalAddress res = start;
        if (bitmap_tail >= bitmap_len)
        {
            assert(false);
            LOG(WARNING) << "shared memory space run out";
        }

        if (bitmap[bitmap_tail] == false)
        {
            bitmap[bitmap_tail] = true;
            res.offset += bitmap_tail * define::kChunkSize;

            bitmap_tail++;
        }
        else
        {
            LOG(FATAL) << "TODO:";
        }

        return res;
    }

    void free_chunk(const GlobalAddress &addr)
    {
        bitmap[(addr.offset - start.offset) / define::kChunkSize] = false;
    }

private:
    GlobalAddress start;

    bool *bitmap;
    size_t bitmap_len;
    size_t bitmap_tail;
};

class GlobalAllocatorWrapper : public mem::BaseAllocator
{
public:
    using pointer = std::shared_ptr<GlobalAllocatorWrapper>;
    GlobalAllocatorWrapper(const GlobalAddress &start, size_t size)
        : alloc_(start, size)
    {
    }
    void *alloc(size_t size, size_t alignment) override
    {
        if (likely(size <= define::kChunkSize))
        {
            auto *ret = (void *) alloc_.alloc_chunck().val;
            DCHECK_EQ((uint64_t) ret % alignment, 0)
                << "** alignment violation";
            return ret;
        }
        return nullptr;
    }
    void free(void *addr) override
    {
        alloc_.free_chunk(GlobalAddress(addr));
    }
    void free(void *addr, size_t size) override
    {
        DCHECK_GE(size, define::kChunkSize);
        return free(addr);
    }

private:
    GlobalAllocator alloc_;
};