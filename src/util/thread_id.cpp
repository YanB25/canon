#include "util/thread_id.h"

#include <atomic>

namespace util
{
std::atomic<uint64_t> __thread_id_allocator{0};
}