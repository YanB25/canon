#pragma once
#include <atomic>
#include <utility>

#include "glog/logging.h"
#include "util/ASM.h"
#include "util/Debug.h"

namespace util
{
class RWLock
{
public:
    void write_lock() noexcept
    {
        while (true)
        {
            // read to reduce cache coherence
            while (is_locked())
            {
                util::asms::cpu_relax();
            }

            uint64_t expect = 0;
            uint64_t target = construct(0, 1);
            if (reader_writer_.compare_exchange_strong(
                    expect, target, std::memory_order_acquire))
            {
                break;
            }
        }
    }
    void write_unlock() noexcept
    {
        DCHECK(is_locked());
        DCHECK_EQ(writer(), 1);
        DCHECK_EQ(reader(), 0);
        reader_writer_.store(0, std::memory_order_release);
    }
    void read_lock() noexcept
    {
        while (true)
        {
            uint64_t val = load_val();
            auto [reader, writer] = destruct(val);
            if (writer > 0)
            {
                util::asms::cpu_relax();
                continue;
            }

            // writer() == 0
            uint64_t target = construct(reader + 1, writer);
            if (reader_writer_.compare_exchange_strong(
                    val, target, std::memory_order_acquire))
            {
                break;
            }
        }
    }
    void read_unlock() noexcept
    {
        DCHECK(is_locked());
        DCHECK_EQ(writer(), 0);
        DCHECK_GT(reader(), 0);
        auto val =
            reader_writer_.fetch_sub(1ull << 32, std::memory_order_release);
        if (debug())
        {
            auto [reader, writer] = destruct(val);
            DCHECK_EQ(writer, 0);
        }
    }
    bool try_write_lock() noexcept
    {
        uint64_t val = load_val();
        if (val != 0)
        {
            return false;
        }
        // val == 0
        uint64_t target = construct(0, 1);
        return reader_writer_.compare_exchange_strong(
            val, target, std::memory_order_acquire);
    }

    bool try_read_lock() noexcept
    {
        uint64_t val = load_val();
        auto [reader, writer] = destruct(val);
        if (writer != 0)
        {
            return false;
        }
        uint64_t target = construct(reader + 1, writer);
        return reader_writer_.compare_exchange_strong(
            val, target, std::memory_order_acquire);
    }

private:
    std::atomic<uint64_t> reader_writer_{0};

    __attribute__((always_inline)) bool is_locked() const
    {
        return reader_writer_.load(std::memory_order_relaxed) != 0;
    }

    __attribute__((always_inline)) std::pair<uint32_t, uint32_t> destruct(
        uint64_t val) const
    {
        uint32_t reader = val >> 32;
        uint32_t writer = (val << 32) >> 32;
        return {reader, writer};
    }
    __attribute__((always_inline)) uint64_t load_val() const
    {
        return reader_writer_.load(std::memory_order_relaxed);
    }
    __attribute__((always_inline)) uint32_t writer() const
    {
        return (load_val() << 32) >> 32;
    }
    __attribute__((always_inline)) uint32_t reader() const
    {
        return load_val() >> 32;
    }
    __attribute__((always_inline)) uint64_t construct(uint32_t reader,
                                                      uint32_t writer)
    {
        return (((uint64_t) reader) << 32) | writer;
    }
};

}  // namespace util