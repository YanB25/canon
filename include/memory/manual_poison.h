#pragma once
#include "./asan_interfaces.h"
#include "glog/logging.h"

constexpr static bool kEnablePoison = false;
constexpr static bool kReportPoison = false;

namespace memory
{
__attribute__((always_inline)) inline void poison_memory_region(
    void const volatile *addr, size_t size)
{
    if constexpr (kEnablePoison)
    {
        LOG_IF(INFO, kReportPoison)
            << "[ASAN] Poisoning " << (void *) addr << ", size: " << size;
        ASAN_POISON_MEMORY_REGION(addr, size);
    }
}

__attribute__((always_inline)) inline void unpoison_memory_region(
    void const volatile *addr, size_t size)
{
    if constexpr (kEnablePoison)
    {
        LOG_IF(INFO, kReportPoison)
            << "[ASAN] Releasing " << (void *) addr << ", size: " << size;
        ASAN_UNPOISON_MEMORY_REGION(addr, size);
    }
}

}  // namespace memory
