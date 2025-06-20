#pragma once
#include "PerThread.h"
#include "util/ASM.h"
#include "util/lock/RWLock.h"

namespace util::synchronize
{
struct IRequest
{
    bool is_valid_{false};
    constexpr bool is_valid() const
    {
        return is_valid_;
    }
    void set_invalid()
    {
        is_valid_ = false;
    }
    void set_valid()
    {
        is_valid_ = true;
    }
};
struct IResponse
{
    bool is_finished_{false};
    constexpr bool is_finished() const
    {
        return is_finished_;
    }
    void set_finish()
    {
        is_finished_ = true;
    }
    void set_unfinish()
    {
        is_finished_ = false;
    }
};

template <typename State, typename Request, typename Response, typename ExecF>
class FlatCombining
{
public:
    Response &execute(const Request &req)
    {
        // 1) submit request
        auto tid = util::get_thread_id();
        auto &my_req = reqs_[tid];
        auto &my_resp = resps_[tid];
        my_resp.set_unfinish();
        CHECK(!my_req.is_valid());
        CHECK(req.is_valid());
        my_req = req;

        // 2) wait or be combiner
        while (true)
        {
            if (my_resp.is_finished())
            {
                return my_resp;
            }
            else
            {
                if (lock_.try_write_lock())
                {
                    combine();
                    lock_.write_unlock();
                    return my_resp;
                }
                else
                {
                    util::asms::cpu_relax();
                }
            }
        }
    }

    void combine()
    {
        for (size_t i = 0; i < kMaxAppThread; ++i)
        {
            auto &req = reqs_[i];
            if (req.is_valid())
            {
                resps_[i] = ExecF{}(&state_, req);
                req.set_invalid();
                resps_[i].set_finish();
            }
        }
    }

private:
    AlignedHeapArray<Request, kMaxAppThread> reqs_;
    AlignedHeapArray<Response, kMaxAppThread> resps_;
    State state_;
    RWLock lock_;
};
}  // namespace util::synchronize