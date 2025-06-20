// https://rigtorp.se/ringbuffer/
#pragma once
#include <atomic>
#include <vector>

#include "util/Likely.h"

namespace util::synchronize
{
// SPSC
template <typename T>
class RingBuffer
{
public:
    using my_size_t = size_t;
    RingBuffer(size_t capacity)
    {
        data_.resize(capacity);
    }
    bool push(const T &t)
    {
        const auto write_idx = write_idx_.load(std::memory_order_relaxed);
        auto next_write_idx = write_idx + 1;
        if (unlikely(next_write_idx == (my_size_t) data_.size()))
        {
            next_write_idx = 0;
        }
        // maybe full, but maybe false positive
        if (unlikely(next_write_idx == cached_read_idx_))
        {
            cached_read_idx_ = read_idx_.load(std::memory_order_acquire);
            if (unlikely(next_write_idx == cached_read_idx_))
            {
                return false;
            }
        }
        // emplace to index
        data_[write_idx] = t;

        write_idx_.store(next_write_idx, std::memory_order_release);
        return true;
    }
    std::optional<T> pop()
    {
        // place on stack
        // so that NRVO is ensured to perform
        std::optional<T> opt;

        const auto read_idx = read_idx_.load(std::memory_order_relaxed);
        if (unlikely(read_idx == cached_write_idx_))
        {
            cached_write_idx_ = write_idx_.load(std::memory_order_acquire);
            if (unlikely(read_idx == cached_write_idx_))
            {
                return opt;
            }
        }

        opt.emplace(std::move(data_[read_idx]));

        auto next_read_idx = read_idx + 1;
        if (unlikely(next_read_idx == (my_size_t) data_.size()))
        {
            next_read_idx = 0;
        }
        read_idx_.store(next_read_idx, std::memory_order_release);
        return opt;
    }
    bool maybe_empty() const
    {
        return read_idx_.load(std::memory_order_relaxed) ==
               write_idx_.load(std::memory_order_relaxed);
    }

private:
    std::vector<T> data_;
    alignas(64) std::atomic<my_size_t> read_idx_{0};
    alignas(64) std::atomic<my_size_t> write_idx_{0};
    alignas(64) my_size_t cached_read_idx_{0};
    alignas(64) my_size_t cached_write_idx_{0};
};
}  // namespace util::synchronize