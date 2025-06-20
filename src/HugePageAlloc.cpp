#include "HugePageAlloc.h"

#include <glog/logging.h>
#include <memory.h>
#include <sys/mman.h>

#include <cstdint>

#include "util/ProcessCleaner.h"
#include "util/Util.h"

void *hugePageAlloc(size_t size)
{
    size = ROUND_UP(size, 2_MB);

    void *res = mmap(NULL,
                     size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                     -1,
                     0);
    if (res == MAP_FAILED)
    {
        if constexpr (debug())
        {
            PLOG(WARNING) << getIP() << " mmap failed for size " << size;
        }
        return nullptr;
    }

    if (size >= 1_GB)
    {
        ProcessCleaner::ins().reg_alloc(res, size);
    }
    madvise(res, size, MADV_DONTDUMP);

    return res;

    // void *aligned_ptr;
    // int alignment = 64;
    // int result = posix_memalign(&aligned_ptr, alignment, size);
    // LOG_IF(FATAL, result != 0) << "Failed to posix_memalign";
    // return CHECK_NOTNULL(aligned_ptr);
}

bool hugePageFree(void *ptr, size_t size)
{
    size_t align = 2_MB;
    CHECK((uint64_t) ptr % align == 0);
    size = ROUND_UP(size, align);
    CHECK(size % align == 0);
    if (munmap(ptr, size))
    {
        PLOG(ERROR) << "failed to free huge page";
        return false;
    }
    return true;
    // std::ignore = size;
    // free(ptr);
    // return true;
}