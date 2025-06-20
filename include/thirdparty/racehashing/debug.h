#pragma once
#include <iostream>

#include "GlobalAddress.h"
#include "PerThread.h"
#include "util/History.h"
#include "util/IRdmaAdaptor.h"
#include "util/Pre.h"
namespace hash
{
constexpr static bool kEnableHistory = true;

struct HashKey
{
    uint16_t nid;
    uint16_t tid;
    uint16_t cid;
    uint64_t value;
    bool operator==(const HashKey &rhs) const
    {
        return nid == rhs.nid && tid == rhs.tid && cid == rhs.cid &&
               value == rhs.value;
    }
    bool operator!=(const HashKey &rhs) const
    {
        return !(*this == rhs);
    }
};
inline std::ostream &operator<<(std::ostream &os, const HashKey &h)
{
    os << "{HashKey nid: " << (int) h.nid << ", tid: " << (int) h.tid
       << ", cid: " << (int) h.cid << ", value: " << h.value << "}";
    return os;
}

enum UserAction
{
    kUpsert,
    kQuery,
    kDelete,
    kEvent,
};
inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] ::hash::UserAction e)
{
    switch (e)
    {
    case ::hash::kUpsert:
        os << "kUpsert";
        break;
    case ::hash::kQuery:
        os << "kQuery";
        break;
    case ::hash::kDelete:
        os << "kDelete";
        break;
    case ::hash::kEvent:
        os << "kEvent";
        break;
    default:
        os << "Unknown ::hash::UserAction(" << (int) e << ")";
    }
    return os;
}

enum RMAction
{
    kCAS,
    kWrite,
    kRead,
    kReadMismatch,
};
inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] ::hash::RMAction e)
{
    switch (e)
    {
    case ::hash::kCAS:
        os << "kCAS";
        break;
    case ::hash::kWrite:
        os << "kWrite";
        break;
    case ::hash::kRead:
        os << "kRead";
        break;
    case ::hash::kReadMismatch:
        os << "kReadMismatch";
        break;
    default:
        os << "Unknown ::hash::RMAction(" << (int) e << ")";
    }
    return os;
}

struct Record
{
    UserAction ma;
    RMAction ua;
    GlobalAddress raddr;
    GlobalAddress raddr2{};
    HashKey key;
    uint8_t slot_val{};
    std::string msg{};
};

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::hash::Record &v)
{
    os << "{Record ";
    os << "user: " << util::pre(v.ma);
    os << ", action: " << util::pre(v.ua);
    os << ", raddr: " << util::pre(v.raddr);
    os << ", raddr2: " << util::pre(v.raddr2);
    os << ", key: " << util::pre(v.key);
    os << ", slot_val: " << (void *) (uint64_t) v.slot_val;
    os << "}";
    return os;
}

extern util::TL_History<Record> history;

}  // namespace hash