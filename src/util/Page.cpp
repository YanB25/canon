#include "util/Page.h"

#include <algorithm>

#include "DSM.h"

namespace util
{
Page::Page(DSM *dsm, size_t size) noexcept : dsm_(dsm)
{
    buffer_ = alloc(size);
}
Page::Page(size_t size) noexcept : dsm_(nullptr)
{
    buffer_ = alloc(size);
}
Page::Page() noexcept : Page(0)
{
}
Page::Page(const Page &rhs) noexcept
{
    dsm_ = rhs.dsm_;
    buffer_ = alloc(rhs.size());
    std::copy(rhs.buffer_.buffer_begin(),
              rhs.buffer_.buffer_end(),
              buffer_.buffer_begin());
}
Page::Page(Page &&other) noexcept
{
    swap(*this, other);
}
Page::~Page()
{
    free(buffer_);
}
Page &Page::operator=(Page other)
{
    swap(*this, other);
    return *this;
}

Buffer Page::alloc(size_t size)
{
    if (likely(size != 0))
    {
        if (likely(dsm_ != nullptr))
        {
            return dsm_->get_rdma_buffer(size);
        }
        else
        {
            return Buffer(new char[size], size);
        }
    }
    return Buffer(nullptr, 0);
}
void Page::free(Buffer &buffer)
{
    if (likely(dsm_ != nullptr))
    {
        if (likely(buffer.buffer != nullptr))
        {
            dsm_->put_rdma_buffer(std::move(buffer));
        }
    }
    else
    {
        if (unlikely(buffer.buffer != nullptr && buffer.size != 0))
        {
            delete[] buffer.buffer;
        }
    }
}

size_t Page::from(const char *from_buffer, size_t size)
{
    auto cpy_size = std::min(size, buffer_.size);
    std::copy(from_buffer, from_buffer + cpy_size, buffer_.buffer);
    return cpy_size;
}
size_t Page::to(char *to_buffer, size_t size)
{
    auto cpy_size = std::min(size, buffer_.size);
    std::copy(buffer_.buffer, buffer_.buffer + cpy_size, to_buffer);
    return cpy_size;
}

}  // namespace util