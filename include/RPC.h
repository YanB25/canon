#pragma once
#include <fmt/core.h>

#include <cinttypes>
#include <iostream>

#include "GlobalAddress.h"
#include "util/Hexdump.hpp"
#include "util/Pre.h"

namespace rpc
{
using flag_t = uint64_t;

enum class RPCType : uint8_t
{
    kAlloc
};

inline std::ostream &operator<<(std::ostream &os, RPCType t)
{
    switch (t)
    {
    case RPCType::kAlloc:
        os << "kAlloc";
        break;
    default:
        os << "Unknown";
        break;
    }
    return os;
}

// common to request and response
struct Header
{
    enum RPCType type;
    uint64_t rpc_context;
    uint16_t from_epid;
    uint16_t from_coro_id;
    uint16_t from_nid;
};

inline std::ostream &operator<<(std::ostream &os, const Header &h)
{
    os << fmt::format("{{Header type: {}, ctx: {}}}",
                      util::pre(h.type),
                      (void *) h.rpc_context);
    return os;
}

struct RpcContext
{
    void *data;
};

struct Request
{
    Header hdr;
    char others[];
} __attribute__((packed));

struct AllocRequest
{
    Header hdr;
    uint8_t flags;  // use AllocFlag
    uint32_t size;
} __attribute__((packed));

inline std::ostream &operator<<(std::ostream &os, const AllocRequest &r)
{
    os << "{AllocRequest hdr: " << r.hdr << ", size: " << r.size << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const Request &r)
{
    switch (r.hdr.type)
    {
    case RPCType::kAlloc:
    {
        os << (const AllocRequest &) r;
        break;
    }
    default:
    {
        LOG(FATAL) << "Unknown RPC type " << (int) r.hdr.type << std::endl
                   << util::Hexdump(&r, sizeof(r));
    }
    }
    return os;
}

struct AllocResponse
{
    Header hdr;
    uint64_t addr;
} __attribute__((packed));

inline std::ostream &operator<<(std::ostream &os, const AllocResponse &r)
{
    os << fmt::format("{{AllocResp hdr: {}, addr: {}}}",
                      util::pre(r.hdr),
                      util::pre(GlobalAddress((void *) r.addr)));
    return os;
}

struct Response
{
    Header hdr;
    char others[];
} __attribute__((packed));

inline std::ostream &operator<<(std::ostream &os, const Response &r)
{
    switch (r.hdr.type)
    {
    case RPCType::kAlloc:
    {
        os << (const AllocResponse &) r;
        break;
    }
    default:
    {
        LOG(FATAL) << "Unknown RPC type: " << (int) r.hdr.type;
    }
    }
    return os;
}

}  // namespace rpc