#include "util/ProcessCleaner.h"

#include <csignal>

#include "Common.h"
#include "HugePageAlloc.h"
#include "glog/logging.h"
#include "util/Pre.h"

void ProcessCleaner::reg_alloc(void *mem, size_t size)
{
    if constexpr (::config::kEnableAbrtHandler)
    {
        std::lock_guard<std::mutex> lk(mu_);
        mem_.emplace_back(mem, size);
    }
}

void ProcessCleaner::do_cleanup()
{
    if constexpr (::config::kEnableAbrtHandler)
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &[addr, size] : mem_)
        {
            LOG(INFO) << "[process] munmap " << addr << " size " << size;
            bool succ = hugePageFree(addr, size);
            if (!succ)
            {
                LOG(ERROR) << "Failed to free " << addr << ", " << size
                           << std::endl;
            }
        }
        mem_.clear();
        LOG(INFO) << "[process] clean up finished.";
    }
}

auto cared_signals = {SIGABRT};

void signal_handler(int sig)
{
    LOG(WARNING) << "[process] handling signal(" << sig
                 << "). If you meet any problem in the "
                    "following procedure (or using gdb), set "
                 << PRE(::config::kEnableAbrtHandler) << " to false";

    ProcessCleaner::ins().do_cleanup();

    LOG(INFO) << "[process] handle signal(" << sig << ") finished.";
}

void register_signal_handlers()
{
    LOG(WARNING) << "[process] register signal handler for SIGABRT";
    for (int s : cared_signals)
    {
        signal(s, signal_handler);
    }
}