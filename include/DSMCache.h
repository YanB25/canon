#pragma once
#if !defined(_CACHE_H_)
#define _CACHE_H_

#include "DSMConfig.h"
#include "HugePageAlloc.h"

struct DSMCacheConfig;
class DSMCache
{
public:
    DSMCache(const DSMCacheConfig &cache_config);

    uint64_t data;
    uint64_t size;

private:
};

struct Buffer
{
    Buffer() : buffer(nullptr), size(0)
    {
    }
    Buffer(char *buffer, size_t size) : buffer(buffer), size(size)
    {
    }
    // Buffer(char *buffer, size_t size, void *debug = nullptr)
    //     : buffer(buffer), size(size), debug_leak_(debug)
    // {
    // }
    friend void swap(Buffer &lhs, Buffer &rhs) noexcept
    {
        using std::swap;
        swap(lhs.buffer, rhs.buffer);
        swap(lhs.size, rhs.size);
        // swap(lhs.debug_leak_, rhs.debug_leak_);
    }
    Buffer(Buffer &&rhs)
    {
        swap(*this, rhs);
    }
    Buffer &operator=(Buffer &&rhs)
    {
        swap(*this, rhs);
        return *this;
    }
    char *buffer_begin() const
    {
        return buffer;
    }
    char *buffer_end() const
    {
        return buffer + size;
    }
    Buffer(const Buffer &rhs) = delete;
    Buffer &operator=(const Buffer &) = delete;
    void assert_contains(const void *buf, size_t sz)
    {
        DCHECK_LE(buffer_begin(), buf)
            << "** DMA-able buffer underflow: buf is " -
                   ((uint64_t) buffer_begin() - (uint64_t) buf)
            << " bytes behind.";
        DCHECK_LE((uint64_t) buf + sz, (uint64_t) buffer_end())
            << "** DMA-able buffer overflow: the last byte is "
            << ((uint64_t) buf + sz - (uint64_t) buffer_end())
            << " bytes after.";
    }

    /**
     * Buffer holds resources, so ideally it is only movable.
     * However, due to history reason, it is complex to remove all the copy of
     * buffer.
     * Therefore, allow .clone() call to do the copy. Use it with care!
     */
    Buffer clone() const
    {
        return Buffer(buffer, size);
        // return Buffer(buffer, size, debug_leak_);
    }

    char *buffer{nullptr};
    size_t size{0};
    // void *debug_leak_{nullptr};
};
inline std::ostream &operator<<(std::ostream &os, const Buffer &buf)
{
    os << "{Buffer base: " << (void *) buf.buffer << ", len: " << buf.size
       << "}";
    return os;
}

inline void validate_buffer_not_overlapped(const Buffer &lhs, const Buffer &rhs)
{
    // https://stackoverflow.com/questions/325933/determine-whether-two-date-ranges-overlap
    auto start_1 = (uint64_t) lhs.buffer;
    auto end_1 = (uint64_t) start_1 + lhs.size;
    auto start_2 = (uint64_t) rhs.buffer;
    auto end_2 = (uint64_t) start_2 + rhs.size;
    // exclusive
    bool overlap = (start_1 < end_2) && (start_2 < end_1);
    CHECK(!overlap) << "Buffer_1 [" << (void *) start_1 << ", "
                    << (void *) end_1 << ") v.s. buffer_2 [" << (void *) start_2
                    << ", " << (void *) end_2 << "). Overlapped.";
}
inline bool test_buffer_not_overlapped(const Buffer &lhs, const Buffer &rhs)
{
    // https://stackoverflow.com/questions/325933/determine-whether-two-date-ranges-overlap
    auto start_1 = (uint64_t) lhs.buffer;
    auto end_1 = (uint64_t) start_1 + lhs.size;
    auto start_2 = (uint64_t) rhs.buffer;
    auto end_2 = (uint64_t) start_2 + rhs.size;
    // exclusive
    bool overlap = (start_1 < end_2) && (start_2 < end_1);
    return !overlap;
}
inline void validate_buffer_not_overlapped(const std::vector<Buffer> &buffers)
{
    for (size_t i = 0; i < buffers.size(); ++i)
    {
        for (size_t j = i + 1; j < buffers.size(); ++j)
        {
            validate_buffer_not_overlapped(buffers[i], buffers[j]);
        }
    }
}

inline void validate_buffer_contain(const Buffer &container, const Buffer &sub)
{
    CHECK_LE(container.buffer_begin(), sub.buffer_begin());
    CHECK_LE(sub.buffer_end(), container.buffer_end());
}

#endif  // _CACHE_H_
