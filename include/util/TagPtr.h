#pragma once

#include <atomic>
#include <iostream>

#include "avis/sizeclass.h"

namespace util
{
union UTaggedPtr
{
    uint64_t val;
    struct
    {
        uint8_t unused_1;
        uint8_t unused_2;
        uint32_t unused_3;
        uint8_t u8_l;
        uint8_t u8_h;
    } __attribute__((packed));
    struct
    {
        uint16_t unused_4;
        uint32_t unused_5;
        uint16_t u16;
    } __attribute__((packed));
};

template <typename T>
class TaggedPtrImpl
{
public:
    constexpr static uintptr_t kMask = ~(1ull << 48);
    TaggedPtrImpl(T *ptr, uint8_t _u8_h, uint8_t _u8_l)
    {
        set(ptr, _u8_h, _u8_l);
    }
    TaggedPtrImpl(T *_ptr, uint16_t _u16)
    {
        set(_ptr, _u16);
    }
    TaggedPtrImpl(T *_ptr)
    {
        set(_ptr, 0);
    }
    TaggedPtrImpl()
    {
        set(nullptr, 0);
    }
    TaggedPtrImpl(uint64_t val)
    {
        set_val(val);
    }

    T *ptr() const
    {
        // sign extend first to make the pointer canonical
        return (T *) (((intptr_t) utagged_ptr_.val << 16) >> 16);
    }
    void set_ptr(void *_ptr)
    {
        set(_ptr, u16());
    }
    uint16_t u16() const
    {
        return utagged_ptr_.u16;
    }
    void set_u16(uint16_t _u16)
    {
        utagged_ptr_.u16 = _u16;
    }
    /**
     * @brief the higher stolen 8 bit higher
     *
     * @return uint8_t
     */
    uint8_t u8_h() const
    {
        return utagged_ptr_.u8_h;
    }
    /**
     * @brief the lower stolen 8 bit lower
     *
     * @return uint8_t
     */
    uint8_t u8_l() const
    {
        return utagged_ptr_.u8_l;
    }

    void set_u8_h(uint8_t _u8_h)
    {
        utagged_ptr_.u8_h = _u8_h;
    }
    void set_u8_l(uint8_t _u8_l)
    {
        utagged_ptr_.u8_l = _u8_l;
    }
    bool cas(TaggedPtrImpl<T> &expected, const TaggedPtrImpl<T> &desired)
    {
        std::atomic<uint64_t> *atm =
            (std::atomic<uint64_t> *) &utagged_ptr_.val;
        return atm->compare_exchange_strong(expected.utagged_ptr_.val,
                                            desired.utagged_ptr_.val,
                                            std::memory_order_acq_rel);
    }
    uint64_t val() const
    {
        return utagged_ptr_.val;
    }
    void set_val(uint64_t val)
    {
        utagged_ptr_.val = val;
    }
    bool operator==(const TaggedPtrImpl<T> &rhs) const
    {
        return utagged_ptr_.val == rhs.utagged_ptr_.val;
    }

private:
    void set(void *_ptr, uint16_t _u16)
    {
        utagged_ptr_.val = (uint64_t) _ptr;
        utagged_ptr_.u16 = _u16;
        DCHECK_EQ(ptr(), _ptr);
        DCHECK_EQ(u16(), _u16);
    }
    void set(void *_ptr, uint8_t _u8_h, uint8_t _u8_l)
    {
        utagged_ptr_.val = (uint64_t) _ptr;
        utagged_ptr_.u8_h = _u8_h;
        utagged_ptr_.u8_l = _u8_l;
        DCHECK_EQ(ptr(), _ptr);
        DCHECK_EQ(u8_h(), _u8_h);
        DCHECK_EQ(u8_l(), _u8_l);
    }
    UTaggedPtr utagged_ptr_;
};
template <typename T>
inline std::ostream &operator<<(std::ostream &os, const TaggedPtrImpl<T> &ptr)
{
    auto flags = os.flags();
    os << "u8_h: " << std::hex << (int) ptr.u8_h()
       << ", u8_l: " << (int) ptr.u8_l() << ", u_16: " << (int) ptr.u16()
       << ", ptr: " << (void *) ptr.ptr();
    os.flags(flags);
    return os;
}

using TaggedPtr = util::TaggedPtrImpl<void>;

static_assert(
    avis::SizeClass{}.size_class().size() <=
        std::numeric_limits<decltype(util::TaggedPtr{}.u8_l())>::max(),
    "size class overflowed");

}  // namespace util