#pragma once

#include <atomic>
#include <cinttypes>

namespace util
{
// globally unique
extern std::atomic<uint64_t> __thread_id_allocator;
inline uint64_t get_thread_id()
{
    static thread_local uint64_t thread_id = __thread_id_allocator.fetch_add(1);
    return thread_id;
}

}  // namespace util