#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

#include "GlobalAddress.h"
#include "GlobalAllocator.h"
#include "RPC.h"
#include "memory/allocator.h"
#include "memory/block_allocator.h"

class DSM;

class Worker
{
public:
    using AllocRequest = rpc::AllocRequest;
    using AllocResponse = rpc::AllocResponse;
    Worker(DSM *dsm,
           size_t wid,
           void *dsm_base_addr,
           ::mem::BaseAllocator::pointer dsm_allocator);

    void signal_exit()
    {
        exited_.store(true);
    }
    void join()
    {
        if (t_.joinable())
        {
            t_.join();
        }
    }
    ~Worker()
    {
        join();
    }
    void launch()
    {
        t_ = std::thread(&Worker::work, this);
    }

    void handle_request_alloc(const rpc::AllocRequest &req);

private:
    void work();

    DSM *dsm_{};
    size_t wid_{};
    void *dsm_base_addr_{};

    mem::BaseAllocator::pointer alloc_;

    std::atomic<bool> exited_{false};
    std::thread t_;
};
