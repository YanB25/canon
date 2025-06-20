#pragma once

#include <atomic>
#include <thread>

#include "util/ASM.h"

namespace util
{
/**
 * Performance:
 * The performance of MCSLock is constent: it does not degrade upon many
 * threads. However, the performance of MCSLock is also suboptimal: We find that
 * it beats other locks only when thread_nr >= 28.
 *
 */
class MCSLock
{
public:
    MCSLock()
    {
        tail_.store(nullptr);
    }
    bool try_write_lock()
    {
        write_lock();
        return true;
    }
    bool try_read_lock()
    {
        read_lock();
        return true;
    }

    void write_lock()
    {
        // getAndSet
        QNode *pred = tail_.load(std::memory_order_relaxed);
        while (true)
        {
            if (tail_.compare_exchange_strong(
                    pred, &my_node_, std::memory_order_acquire))
            {
                break;
            }
            util::asms::cpu_relax();
        }

        if (pred)
        {
            my_node_.locked = true;
            pred->next.store(&my_node_, std::memory_order_relaxed);
            while (my_node_.locked)
            {
                util::asms::cpu_relax();
            }
        }
    }
    void read_lock()
    {
        return write_lock();
    }

    void read_unlock()
    {
        return write_unlock();
    }

    void write_unlock()
    {
        auto next = my_node_.next.load(std::memory_order_relaxed);
        if (next == nullptr)
        {
            QNode *ref = &my_node_;
            // confirmed that no follower
            if (tail_.compare_exchange_strong(
                    ref, nullptr, std::memory_order_release))
            {
                return;
            }
            else
            {
                // follower exists
                while (my_node_.next.load(std::memory_order_relaxed) == nullptr)
                {
                    util::asms::cpu_relax();
                }
            }
        }
        my_node_.next.load(std::memory_order_relaxed)->locked = false;
        my_node_.next = nullptr;
    }

private:
    struct QNode
    {
        QNode() : locked(false), next(nullptr)
        {
        }
        bool locked;
        std::atomic<QNode *> next;
    };

    std::atomic<QNode *> tail_;
    static thread_local QNode my_node_;
};

}  // namespace util