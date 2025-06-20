#pragma once
#include <atomic>
#include <cinttypes>
#include <vector>

#include "glog/logging.h"
#include "util/Rand.h"
#include "util/UP.h"
#include "util/Util.h"

namespace util::synchronize
{
// Thread-safe and lock-free bit set
class Bitset
{
public:
    // each uint64_t is call a slot, which has 64 bits
    // (idx 64) MSB ..... LSB (idx 0)
    using slot_t = uint64_t;
    constexpr static slot_t kAllResetSlot = slot_t();
    constexpr static slot_t kAllSetSlot = ~kAllResetSlot;

    Bitset(size_t bit_nr) : bit_nr_(bit_nr)
    {
        auto expect_slot_nr =
            (bit_nr + bit_nr_per_slot() - 1) / bit_nr_per_slot();
        expect_slot_nr = std::max((size_t) 1, expect_slot_nr);
        set_.resize(expect_slot_nr);

        actual_bit_nr_ = expect_slot_nr * bit_nr_per_slot();
    }
    constexpr static size_t bit_nr_per_slot()
    {
        return sizeof(slot_t) * 8;
    }
    slot_t read_slot(size_t slot_id)
    {
        auto &slot = get_atomic(slot_id);
        return slot.load(std::memory_order_relaxed);
    }
    // idempotent set
    void set(size_t idx)
    {
        auto [slot, bit_idx] = locate(idx);

        slot_t slot_val = slot.load(std::memory_order_relaxed);
        // if other helps us to set the same bit, also okay and exit
        while (!slot_test(slot_val, bit_idx))
        {
            slot_t expect = get_slot_set(slot_val, bit_idx);

            if (slot.compare_exchange_strong(
                    slot_val, expect, std::memory_order_acq_rel))
            {
                return;
            }
        }
    }
    void reset(size_t idx)
    {
        auto [slot, bit_idx] = locate(idx);
        slot_t slot_val = slot.load(std::memory_order_relaxed);

        while (slot_test(slot_val, bit_idx))
        {
            slot_t expect = get_slot_reset(slot_val, bit_idx);
            if (slot.compare_exchange_strong(
                    slot_val, expect, std::memory_order_acq_rel))
            {
                return;
            }
        }
    }

    bool test(size_t idx) const
    {
        auto [slot, bit_idx] = locate(idx);
        slot_t slot_val = slot.load(std::memory_order_relaxed);
        return slot_test(slot_val, bit_idx);
    }
    // WARNING: this may be inaccurate and slow
    size_t count() const
    {
        size_t ret = 0;
        for (auto slot : set_)
        {
            ret += count_set_bit(slot);
        }
        return ret;
    }
    // success, idx
    std::pair<bool, size_t> try_atomic_random_set_n(size_t slot_idx, size_t n)
    {
        // build expect str
        CHECK_LT(n, sizeof(uint64_t) * 8)
            << "** can not handle such large block";
        uint64_t mask_template = (1ull << n) - 1;  // lower n bits are 1

        auto &slot = get_atomic(slot_idx);
        slot_t slot_val = slot.load(std::memory_order_relaxed);

        for (size_t i = 0; i <= bit_nr_per_slot() - n; ++i)
        {
            uint64_t expect = mask_template << i;
            if ((~slot_val & expect) == expect)
            {
                // match!
                slot_t next_slot_val = slot_val | expect;
                if (slot.compare_exchange_strong(slot_val, next_slot_val))
                {
                    auto ret = to_idx(slot_idx, i);
                    return {true, ret};  // success, idx
                }
            }
        }
        return {false, 0};  // not success, *
    }
    std::optional<size_t> atomic_random_set_n(size_t n)
    {
        while (true)
        {
            auto ret = do_atomic_random_set_n(n);
            if (!ret)
            {
                return std::nullopt;
            }
            if (unlikely(*ret >= bit_nr_))
            {
                continue;
            }
            return ret;
        }
    }
    // precondition:
    // - count() > n
    // postcondition:
    // return an index to the bitset, where
    // this, and the following n-1 bits are atomically transfered from unset to
    // set
    std::optional<size_t> do_atomic_random_set_n(size_t n)
    {
        if (unlikely(n == 1))
        {
            return do_atomic_random_set();
        }
        // fast path
        {
            auto slot_idx = round_robin_idx_.current() % set_.size();
            auto [success, idx] = try_atomic_random_set_n(slot_idx, n);
            if (likely(success))
            {
                return idx;
            }
        }
        // normal path
        for (size_t i = 0; i < 3; ++i)
        {
            round_robin_idx_.current() =
                fast_pseudo_rand_int(0, set_.size() - 1);
            auto slot_idx = round_robin_idx_.current();
            auto [success, idx] = try_atomic_random_set_n(slot_idx, n);
            if (likely(success))
            {
                return idx;
            }
        }

        LOG_FIRST_N(WARNING, 1) << "** Failed to fastly set " << n
                                << " bits under several attempts: conflict is"
                                << "high or empty slot is rare.";

        // slow path:
        size_t slow_path_attempt = 0;
        round_robin_idx_.current() = fast_pseudo_rand_int(0, set_.size() - 1);
        for (size_t i = 0; i < set_.size(); ++i)
        {
            auto slot_idx = round_robin_idx_.current();
            auto [success, idx] = try_atomic_random_set_n(slot_idx, n);
            if (likely(success))
            {
                return idx;
            }

            slow_path_attempt++;
            if (unlikely(slow_path_attempt > bit_nr_))
            {
                return std::nullopt;
            }

            round_robin_idx_.current() =
                (round_robin_idx_.current() + 1) % set_.size();
        }

        return std::nullopt;
    }
    // precontidion: [idx, idx + n) is set
    // postcondition:
    // [idx, idx + n] is unset
    void atomic_unset_n(size_t g_idx, size_t n)
    {
        auto [slot_idx, bit_idx] = explain(g_idx);
        auto &slot = get_atomic(slot_idx);
        uint64_t expect = (1ull << n) - 1;
        expect = expect << bit_idx;
        if constexpr (debug())
        {
            auto slot_val = slot.load(std::memory_order_relaxed);
            if ((slot_val & expect) != expect)
            {
                LOG(FATAL) << "** double unset detected: " << PRE(g_idx) << ", "
                           << PRE(n) << ", slot: " << util::pre_bin(slot_val)
                           << ", unset bits: " << util::pre_bin(expect);
            }
        }
        slot.fetch_sub(expect);
    }
    // precondition: count() > 0
    // postcondition:
    // return an index to the bitset, where
    // this bit is atomically transfered from unset to set
    std::optional<size_t> atomic_random_set()
    {
        // will not get deadlock
        // because in each retry, we set one illegal bit
        // therefore, the same bit will not be set again.
        while (true)
        {
            auto ret = do_atomic_random_set();
            if (!ret)
            {
                return std::nullopt;
            }
            if (unlikely(*ret >= bit_nr_))
            {
                continue;
            }
            return ret;
        }
    }
    // <success, idx, exhausted>
    // if success, idx is the index of the set bit
    // if not success, idx is 0
    // if not success && exhausted, this slot has been all set.
    std::tuple<bool, size_t, bool> try_atomic_random_set_from_slot(
        size_t slot_idx)
    {
        auto &slot = get_atomic(slot_idx);
        slot_t slot_val = slot.load(std::memory_order_relaxed);
        if (likely(slot_val != kAllSetSlot))
        {
            auto bit_idx = first_reset_bit_idx(slot_val);
            slot_t expect = get_slot_set(slot_val, bit_idx);
            if (slot.compare_exchange_strong(
                    slot_val, expect, std::memory_order_acquire))
            {
                // LOG(INFO) << "get_slot_set for " << PRE(slot_idx) << ", "
                //           << PRE(bit_idx) << " before " << (void *) slot_val
                //           << ". expect: " << (void *) expect << "(" << tid
                //           << ")";
                auto ret = to_idx(slot_idx, bit_idx);
                // success, idx, *
                return {true, ret, false};
            }
            else
            {
                // not success, *, not exhuasted (conflict)
                return {false, 0, false};
            }
        }
        else
        {
            // not success, *, exhausted
            return {false, 0, true};
        }
    }
    std::optional<size_t> do_atomic_random_set()
    {
        // fast path: use the same slot (same round_robin_idx_)
        // In the hope that this slot is not conflict.
        // In this way, locality is good
        {
            auto slot_idx = round_robin_idx_.current() % set_.size();
            auto [success, idx, _] = try_atomic_random_set_from_slot(slot_idx);
            if (likely(success))
            {
                return idx;
            }
        }

        // normal path:
        // fast path failed because of
        // a) bucket is empty, or
        // b) conflict with other threads
        // Try to choose a new slots several times that is non-empty and
        // conflict-free
        for (size_t i = 0; i < 3; ++i)
        {
            round_robin_idx_.current() =
                fast_pseudo_rand_int(0, set_.size() - 1);
            auto slot_idx = round_robin_idx_.current();
            auto [success, idx, exhausted] =
                try_atomic_random_set_from_slot(slot_idx);
            if (likely(success))
            {
                return idx;
            }
        }

        LOG_FIRST_N(WARNING, 1)
            << "** Failed to fastly set one bit under several attempts : "
               "conflict is high or empty slot is rare.";

        // slow path:
        // we revert to using scan
        // it is slow but is much more reliable
        size_t slow_path_attempt = 0;
        round_robin_idx_.current() = fast_pseudo_rand_int(0, set_.size() - 1);
        for (size_t i = 0; i < set_.size(); ++i)
        {
            auto slot_idx = round_robin_idx_.current();
            while (true)
            {
                auto [success, idx, exhausted] =
                    try_atomic_random_set_from_slot(slot_idx);
                slow_path_attempt++;
                LOG_IF(FATAL, slow_path_attempt >= 2 * bit_nr_)
                    << "** concurrent bit set attempts more than 2*num_of_bit. "
                       "No empty bit existed.";

                if (likely(success))
                {
                    return idx;
                }
                // use next slot
                if (unlikely(exhausted))
                {
                    // +1 instead of using a random one
                    round_robin_idx_.current() =
                        (round_robin_idx_.current() + 1) % set_.size();
                    break;
                }
            }
        }
        return std::nullopt;
        // LOG(FATAL) << "** Failed to find any reset bit. count: " << count();
    }

    std::pair<size_t, size_t> explain(size_t idx)
    {
        auto slot_idx = idx / bit_nr_per_slot();
        auto bit_idx = idx % bit_nr_per_slot();
        return {slot_idx, bit_idx};
    }

private:
    size_t bit_nr_;
    size_t actual_bit_nr_;
    std::vector<slot_t> set_;

    Perthread<size_t> round_robin_idx_;

    size_t to_idx(size_t slot_idx, size_t bit_idx)
    {
        return slot_idx * bit_nr_per_slot() + bit_idx;
    }

    // count from MSB
    __attribute__((always_inline)) static int first_set_bit_idx(uint64_t val)
    {
        return 63 - __builtin_clzll(val);
    }
    __attribute__((always_inline)) static int first_reset_bit_idx(uint64_t val)
    {
        return 63 - __builtin_clzll(~val);
    }

    __attribute__((always_inline)) static bool slot_test(slot_t slot,
                                                         size_t idx)
    {
        DCHECK_LT(idx, bit_nr_per_slot());
        return slot & (1ull << idx);
    }
    __attribute__((always_inline)) static slot_t get_slot_set(slot_t slot,
                                                              size_t idx)
    {
        DCHECK_LT(idx, bit_nr_per_slot());
        return slot | (1ull << idx);
    }
    __attribute__((always_inline)) static slot_t get_slot_reset(slot_t slot,
                                                                size_t idx)
    {
        DCHECK_LT(idx, bit_nr_per_slot());
        return slot & (~(1ull << idx));
    }
    __attribute__((always_inline)) static size_t count_set_bit(uint64_t val)
    {
        return __builtin_popcountll(val);
    }
    // [ref to slot, bit_idx]
    __attribute__((always_inline)) std::pair<std::atomic<slot_t> &, size_t>
    locate(size_t idx)
    {
        DCHECK_LT(idx, actual_bit_nr_);
        auto slot_idx = idx / bit_nr_per_slot();
        auto bit_idx = idx % bit_nr_per_slot();
        auto &atm = get_atomic(slot_idx);
        return {atm, bit_idx};
    }
    __attribute__((always_inline))
    std::pair<const std::atomic<slot_t> &, size_t>
    locate(size_t idx) const
    {
        DCHECK_LT(idx, actual_bit_nr_);
        auto slot_idx = idx / bit_nr_per_slot();
        auto bit_idx = idx % bit_nr_per_slot();
        auto &atm = get_atomic(slot_idx);
        return {atm, bit_idx};
    }

    std::atomic<slot_t> &get_atomic(size_t slot_idx)
    {
        DCHECK_LT(slot_idx, set_.size());
        auto *atm_ptr = (std::atomic<slot_t> *) &set_[slot_idx];
        return *atm_ptr;
    }
    const std::atomic<slot_t> &get_atomic(size_t slot_idx) const
    {
        DCHECK_LT(slot_idx, set_.size());
        const auto *atm_ptr = (const std::atomic<slot_t> *) &set_[slot_idx];
        return *atm_ptr;
    }
};
}  // namespace util::synchronize