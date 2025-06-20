#pragma once
#include <cstddef>
#include <map>
#include <mutex>

#include "GlobalAddress.h"
#include "glog/logging.h"
#include "source_location/source_location.hpp"
namespace avis
{
struct MMDesc
{
    size_t bytes;
    bool read;
    bool write;
    bool atomic;
};
inline std::ostream &operator<<(std::ostream &os, const MMDesc &d)
{
    os << "{byte: " << d.bytes << ", perm: ";
    if (d.read)
    {
        os << "r";
    }
    if (d.write)
    {
        os << "w";
    }
    if (d.atomic)
    {
        os << "a";
    }
    os << "}";
    return os;
}
enum Op
{
    kRead,
    kWrite,
    kAtomic,
};
inline std::ostream &operator<<(std::ostream &os, Op op)
{
    if (op == kRead)
    {
        os << "r";
    }
    if (op == kWrite)
    {
        os << "w";
    }
    if (op == kAtomic)
    {
        os << "a";
    }
    return os;
}
class Verifier
{
public:
    constexpr static bool kEnable = false;
    ;
    static Verifier &ins()
    {
        static Verifier ins_;
        return ins_;
    }
    void add_mm(GlobalAddress gaddr, size_t size, bool r, bool w, bool atomic)
    {
        if constexpr (kEnable)
        {
            add_mm((void *) gaddr.val, size, r, w, atomic);
        }
    }
    void add_mm(void *addr, size_t size, bool r, bool w, bool atomic)
    {
        if constexpr (kEnable)
        {
            std::lock_guard<std::mutex> lk(mu_);

            if (map_.contains(addr))
            {
                LOG(FATAL) << "[verify] already have addr " << addr << ": "
                           << map_[addr];
            }
            map_[addr] =
                MMDesc{.bytes = size, .read = r, .write = w, .atomic = atomic};
        }
    }
    void del_mm(GlobalAddress addr, size_t size)
    {
        if constexpr (kEnable)
        {
            del_mm((void *) addr.val, size);
        }
    }
    void del_mm(void *addr, size_t size)
    {
        if constexpr (kEnable)
        {
            std::lock_guard<std::mutex> lk(mu_);

            auto it = map_.find(addr);
            if (it == map_.end())
            {
                LOG(FATAL) << "[verify] do not have addr " << addr;
            }
            CHECK_EQ(it->second.bytes, size) << "[verify] size mismatch at "
                                             << addr << ": got " << it->second;
            map_.erase(it);
        }
    }
    void verify(
        Op v_op,
        GlobalAddress v_addr,
        size_t v_size,
        const nostd::source_location &loc = nostd::source_location::current())
    {
        return verify(v_op, (void *) v_addr.val, v_size, loc);
    }
    void verify(
        Op v_op,
        void *v_addr,
        size_t v_size,
        const nostd::source_location &loc = nostd::source_location::current())
    {
        if constexpr (kEnable)
        {
            std::lock_guard<std::mutex> lk(mu_);
            uint64_t v_left = (uint64_t) v_addr;
            uint64_t v_right = (uint64_t) v_addr + v_size;
            // size_t hit = 0;
            for (const auto &[addr, desc] : map_)
            {
                uint64_t begin = (uint64_t) addr;
                uint64_t end = (uint64_t) addr + desc.bytes;
                if (begin <= v_left && end >= v_right)
                {
                    // hit
                    if (v_op == kRead && !desc.read)
                    {
                        LOG(FATAL) << PRE(v_op, v_addr, v_size, loc);
                    }
                    if (v_op == kWrite && !desc.write)
                    {
                        LOG(FATAL) << PRE(v_op, v_addr, v_size, loc);
                    }
                    if (v_op == kAtomic && !desc.atomic)
                    {
                        LOG(FATAL) << PRE(v_op, v_addr, v_size, loc);
                    }
                }
            }
            // CHECK_GE(hit, 1) << "** no permission found for " << v_op << " at
            // "
            //                  << v_addr << " length " << v_size << std::endl
            //                  << " at " << loc;
        }
    }

private:
    Verifier() = default;
    std::mutex mu_;
    std::map<void *, MMDesc> map_;
};
}  // namespace avis