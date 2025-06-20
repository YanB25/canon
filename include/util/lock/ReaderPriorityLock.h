#pragma once
#include <atomic>

#include "PerThread.h"
#include "glog/logging.h"
namespace util
{
// This lock offers *nearly* no overhead under RO case,
// and favours writer.
// Good to use if
// - RO
// - Only a few writers and lots of readers.
class RPLock
{
public:
    void lock() noexcept
    {
        write_lock();
    }
    void unlock() noexcept
    {
        write_unlock();
    }

    bool try_write_lock_phase_1()
    {
        uint64_t expect = write_.load(std::memory_order_relaxed);
        if (expect % 2 == 1)
        {
            return false;
        }
        DCHECK_EQ((expect + 1) % 2, 1);
        if (write_.compare_exchange_strong(
                expect, expect + 1, std::memory_order_acquire))
        {
            return true;
        }
        return false;
    }

    void write_lock() noexcept
    {
        // set CAS write_lock to true
        write_lock_phase_1();
        // wait until all reader leaves
        wait_readers();
    }
    void write_unlock() noexcept
    {
        uint64_t cur = write_.load(std::memory_order_relaxed);
        DCHECK_EQ(cur % 2, 1);
        write_.store(cur + 1, std::memory_order_release);
    }

    void read_lock() noexcept
    {
    retry:
        uint64_t write;
        do
        {
            write = write_.load(std::memory_order_relaxed);
            // write no lock
            if (write % 2 == 0)
            {
                break;
            }
            else
            {
                util::asms::cpu_relax();
            }
        } while (true);

        read_.current().store(true, std::memory_order_acquire);

        // re-read
        uint64_t reread_write = write_.load(std::memory_order_relaxed);
        if (write != reread_write)
        {
            // roll back and retry
            read_.current().store(false, std::memory_order_release);
            goto retry;
        }
        else
        {
            // okay
            return;
        }
    }
    void read_unlock() noexcept
    {
        read_.current().store(false, std::memory_order_release);
    }
    bool try_read_lock() noexcept
    {
        if (is_write_locked())
        {
            return false;
        }
        read_.current().store(true, std::memory_order_acquire);
        return true;
    }

    void wait_readers()
    {
        for (const auto &r : read_)
        {
            while (r.get().load(std::memory_order_relaxed))
            {
                util::asms::cpu_relax();
            }
        }
    }
    void write_lock_phase_1() noexcept
    {
        // fast path
        if (try_write_lock_phase_1())
        {
            return;
        }

        while (true)
        {
            while (is_write_locked())
            {
                util::asms::cpu_relax();
            }

            if (try_write_lock_phase_1())
            {
                return;
            }
            util::asms::cpu_relax();
        }
    }

    constexpr bool is_write_locked() const noexcept
    {
        return write_.load(std::memory_order_relaxed) % 2 == 1;
    }
    constexpr bool is_read_locked() const noexcept
    {
        for (const auto &r : read_)
        {
            if (r.get().load(std::memory_order_relaxed))
            {
                return true;
            }
        }
        return false;
    }
    constexpr bool is_locked() const noexcept
    {
        return is_write_locked() || is_read_locked();
    }

private:
    Perthread<std::atomic<bool>> read_{};
    std::atomic<uint64_t> write_{false};
};
}  // namespace util