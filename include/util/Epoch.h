#pragma once

#include <fmt/core.h>
#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <limits>
#include <mutex>

#include "Common.h"
#include "CoroContext.h"
#include "DSM.h"
#include "GlobalAddress.h"
#include "util/thread_id.h"

// struct EpochMeta
// {
//     uint64_t version;
// };

// class IEpoch
// {
// public:
//     virtual void enter() = 0;
//     virtual void leave() = 0;
//     virtual ~IEpoch() = default;
// };
// class Epoch
// {
// public:
//     Epoch(DSM::pointer dsm,
//           GlobalAddress from_v,
//           std::atomic<uint64_t> &to_v,
//           CoroContext *ctx)
//         : dsm_(dsm), meta_(meta), ctx_(ctx)
//     {
//         meta_buf_ = dsm_->get_rdma_buffer(sizeof(EpochMeta));
//         read_meta();
//     }
//     void read_meta() const
//     {
//         dsm_->prepare_read(
//             meta_buf_.buffer, meta_, sizeof(EpochMeta), false, ctx_);
//         dsm_->commit(ctx_);
//     }

//     void enter()
//     {
//     }
//     void leave()
//     {
//     }

//     ~Epoch()
//     {
//         dsm_->put_rdma_buffer(std::move(meta_buf_));
//     }

// private:
//     DSM::pointer dsm_;
//     GlobalAddress from_v_;
//     std::atomic<uint64_t> &to_v_;
//     CoroContext *ctx_{};

//     Buffer meta_buf_;
// };

struct LEpoch
{
    std::atomic<uint64_t> known;
};

class Epocher
{
public:
    Epocher(std::atomic<uint64_t> &g_epoch) : g_epoch_(g_epoch)
    {
        update();
    }
    void enter()
    {
        update();
    }
    void leave()
    {
        update();
    }
    uint64_t local_epoch() const
    {
        return l_epoch_.load(std::memory_order_relaxed);
    }

private:
    void update()
    {
        auto e = g_epoch_.load(std::memory_order_relaxed);
        if constexpr (debug())
        {
            if (e != l_epoch_.load(std::memory_order_relaxed))
            {
                LOG(INFO) << "[Epoch] client(" << util::get_thread_id()
                          << ") <= " << e;
            }
        }
        l_epoch_.store(e, std::memory_order_relaxed);
    }
    std::atomic<uint64_t> &g_epoch_;
    std::atomic<uint64_t> l_epoch_{};
};

class EpochManager
{
public:
    EpochManager(std::chrono::nanoseconds interval) : interval_(interval)
    {
        last_ = std::chrono::steady_clock::now();
    }

    Epocher *new_instance()
    {
        std::lock_guard<std::mutex> lk(mu_);
        lepoch_.emplace_back(g_epoch_);
        return &(lepoch_.back());
    }

    // Only one thread to call this function periodically.
    void try_poll()
    {
        auto now = std::chrono::steady_clock::now();

        auto elaps = now - last_;
        if (elaps >= interval_)
        {
            // NOTE: must update last_ to += interval
            // do not use last_ = now
            // otherwise, can not get full performance
            last_ = last_ + interval_;
            // update
            g_epoch_.fetch_add(1);
            // LOG(INFO) << "[Epoch] g_epoch <= "
            //           << g_epoch_.load(std::memory_order_relaxed);

            maintain_min();
        }
    }
    void poll(uint64_t e, bool do_maintain_min)
    {
        g_epoch_.store(e, std::memory_order_relaxed);
        if (do_maintain_min)
        {
            maintain_min();
        }
    }
    uint64_t epoch() const
    {
        return g_epoch_.load(std::memory_order_relaxed);
    }
    uint64_t min_epoch() const
    {
        return min_clients_.load(std::memory_order_relaxed);
    }

private:
    std::chrono::nanoseconds interval_;
    std::chrono::steady_clock::time_point last_;

    std::list<Epocher> lepoch_;
    std::mutex mu_;

    std::atomic<uint64_t> g_epoch_{};
    std::atomic<uint64_t> min_clients_{};

    void maintain_min()
    {
        auto min_clients_epoch = min_clients_.load(std::memory_order_relaxed);
        uint64_t min_local = std::numeric_limits<uint64_t>::max();
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto &e : lepoch_)
            {
                auto local = e.local_epoch();
                min_local = std::min(local, min_local);
            }
        }
        if (min_local > min_clients_epoch)
        {
            min_clients_.store(min_local, std::memory_order_relaxed);
            // LOG(INFO) << "[Epoch] min_client <= " << min_local;
        }
    }
};

struct EpochMeta
{
    uint64_t epoch{};
    uint64_t client_nr{};
    uint64_t min_epoch[MAX_MACHINE]{};
};

class ClusterEpochManager
{
public:
    ClusterEpochManager(DSM::pointer dsm,
                        GlobalAddress meta,
                        CoroContext *ctx,
                        std::chrono::nanoseconds interval)
        : dsm_(dsm), meta_(meta), ctx_(ctx), interval_(interval), em_(interval)
    {
        meta_buf_ = dsm_->get_rdma_buffer(sizeof(EpochMeta));
        read_meta();
        join();
        last_ = std::chrono::steady_clock::now();
    }
    void join()
    {
        auto offset = offsetof(EpochMeta, client_nr);
        dsm_->prepare_faa(meta_ + offset,
                          sizeof(uint64_t),
                          1,
                          0,
                          meta_buf_.buffer + offset,
                          false,
                          ctx_);
        dsm_->commit(ctx_);
    }
    Epocher *new_instance()
    {
        return em_.new_instance();
    }

    void read_meta()
    {
        dsm_->prepare_read(
            meta_buf_.buffer, meta_, sizeof(EpochMeta), false, ctx_);
        dsm_->commit(ctx_);
    }

    // this must be only one thread globally
    void try_roll_forward()
    {
        auto now = std::chrono::steady_clock::now();

        auto elaps = now - last_;
        if (elaps >= interval_)
        {
            last_ += interval_;
            cached_epoch_++;

            // update to remote
            auto *meta = (EpochMeta *) meta_buf_.buffer;
            dsm_->prepare_write(
                (char *) &(meta->epoch), meta_, sizeof(uint64_t), false, ctx_);
            dsm_->commit(ctx_);
        }
    }

    void try_poll()
    {
        auto now = std::chrono::steady_clock::now();

        auto elaps = now - last_;
        if (elaps >= interval_)
        {
            last_ += interval_;

            dsm_->prepare_read(
                meta_buf_.buffer, meta_, sizeof(EpochMeta), false, ctx_);
            dsm_->commit(ctx_);

            EpochMeta *meta = (EpochMeta *) meta_buf_.buffer;
            // if (meta->epoch != cached_epoch_)
            // {
            //     DCHECK_GE(meta->epoch, cached_epoch_);
            //     cached_epoch_ = meta->epoch;
            //     poll(cached_epoch_, true);
            // }

            // first, check whether local epoch needs to be updated
            auto local_min_epoch = em_.min_epoch();
        }
    }

    // void poll(uint64_t e, bool do_maintain_min)
    // {
    //     LOG(FATAL) << "This is WRONG. We need to know the min of all clients.
    //     "
    //                   "Therefore, we must push local min to the meta.";
    //     em_.poll(e, do_maintain_min);

    //     if (do_maintain_min)
    //     {
    //         auto cur_min = em_.min_epoch();
    //         if (cur_min != cached_min_epoch_)
    //         {
    //             DCHECK_GE(cur_min, cached_min_epoch_);
    //             auto target_min = cur_min;

    //             auto *meta = (EpochMeta *) meta_buf_.buffer;

    //             while (true)
    //             {
    //                 dsm_->prepare_cas(meta_ + sizeof(uint64_t),
    //                                   sizeof(uint64_t),
    //                                   cached_min_epoch_,
    //                                   0xffffffffffffffff,
    //                                   cur_min,
    //                                   0xffffffffffffffff,
    //                                   &(meta->min_epoch),
    //                                   false,
    //                                   ctx_);
    //                 dsm_->commit(ctx_);

    //                 uint64_t got = meta->min_epoch;
    //                 if (got == cached_min_epoch_)
    //                 {
    //                     // CAS ok
    //                     cached_min_epoch_ = target_min;
    //                 }
    //                 else if (got >= target_min)
    //                 {
    //                     cached_min_epoch_ = got;
    //                     // CAS failed but can finish
    //                     break;
    //                 }
    //             }
    //         }
    //     }
    // }
    uint64_t epoch() const
    {
        return em_.epoch();
    }
    uint64_t min_epoch() const
    {
        return em_.min_epoch();
    }

    ~ClusterEpochManager()
    {
        dsm_->put_rdma_buffer(std::move(meta_buf_));
    }

private:
    DSM::pointer dsm_;
    GlobalAddress meta_;
    CoroContext *ctx_{nullptr};
    std::chrono::nanoseconds interval_;
    std::chrono::steady_clock::time_point last_;

    Buffer meta_buf_;
    EpochManager em_;

    uint64_t cached_epoch_{};
    uint64_t cached_min_epoch_{};
};

// class LocalEpoch
// {
// public:
//     LocalEpoch()
//     {
//     }

// private:
// };