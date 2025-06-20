#pragma once
#include <cinttypes>
#include <cstddef>
#include <iostream>

#include "DSMCache.h"
#include "glog/logging.h"
#include "util/Likely.h"

class DSM;

namespace util
{
struct init_page_t
{
};

static init_page_t init_page_tag;
/**
 * Page could be EITHER a DSM-backed page or a malloc page.
 * DSM-backed page:
 * - DMA-able
 * - use DSM's get_rdma_buffer & put_rdma_buffer to manage memory
 * malloc-backed page:
 * - not DMA-able
 * - use new & delete to manage memory
 */
class Page
{
public:
    Page(DSM *dsm, size_t size) noexcept;
    Page(size_t size) noexcept;
    Page(const Page &rhs) noexcept;
    Page() noexcept;
    ~Page();
    friend void swap(Page &lhs, Page &rhs) noexcept
    {
        using std::swap;
        swap(lhs.buffer_, rhs.buffer_);
        swap(lhs.dsm_, rhs.dsm_);
    }
    // The copy-and-swap-idiom
    // https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
    Page &operator=(Page other);
    Page(Page &&other) noexcept;

    constexpr size_t size() const
    {
        return buffer_.size;
    }

    constexpr char *data() const
    {
        return buffer_.buffer;
    }
    constexpr bool is_DMA() const
    {
        return dsm_ != nullptr;
    }

    size_t from(const char *from_buffer, size_t size);
    size_t to(char *to_buffer, size_t size);

private:
    DSM *dsm_{nullptr};
    Buffer buffer_;

    Buffer alloc(size_t size);
    void free(Buffer &buffer);
};
inline std::ostream &operator<<(std::ostream &os, const Page &p)
{
    os << "{Page " << p.size() << "}";
    return os;
}

}  // namespace util