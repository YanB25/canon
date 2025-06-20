#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "util/Coro.h"
#include "util/RetCode.h"

namespace patronus
{
using id_t = uint64_t;
using rkey_t = uint32_t;
using chrono_time_t = std::chrono::time_point<std::chrono::steady_clock>;
using flag_t = uint64_t;

// force enum to be sizeof(uint8_t)
enum class RpcType : uint8_t
{
    kAcquireRLeaseReq,
    kAcquireWLeaseReq,
    kAcquireNoLeaseReq,  // for debug purpose
    kAcquireLeaseResp,
    kRelinquishReq,
    kRelinquishResp,
    kExtendReq,
    kExtendResp,
    kAdmin,
    kAdminReq,
    kAdminResp,
    kMemoryReq,
    kMemoryResp,
    kTimeSync,
    kMemcpyReq,
    kMemcpyResp,
    kReserved,
};
std::ostream &operator<<(std::ostream &os, const RpcType &t);

inline RpcType get_resp_rpc_type(RpcType send_type)
{
    switch (send_type)
    {
    case RpcType::kAcquireRLeaseReq:
    case RpcType::kAcquireWLeaseReq:
    case RpcType::kAcquireNoLeaseReq:
        return RpcType::kAcquireLeaseResp;
    case RpcType::kRelinquishReq:
        return RpcType::kRelinquishResp;
    case RpcType::kExtendReq:
        return RpcType::kExtendResp;
    case RpcType::kAdminReq:
        return RpcType::kAdminResp;
    case RpcType::kMemoryReq:
        return RpcType::kMemoryResp;
    case RpcType::kMemcpyReq:
        return RpcType::kMemcpyResp;
    default:
        LOG(FATAL) << "** Unknown or invalid request type: " << send_type;
    }
    return RpcType::kReserved;
}

struct ClientID
{
    uint16_t node_id;
    uint16_t thread_id;
    coro_t coro_id;
    uint64_t async_context;  // actually is (AsyncContext*)
    /**
     * only the node_id, thread_id and coro_id is the identity.
     */
    bool is_same(const ClientID &rhs) const
    {
        return node_id == rhs.node_id && thread_id == rhs.thread_id &&
               coro_id == rhs.coro_id;
    }

    // to be safe. use is_same instead
    bool operator==(const ClientID &rhs) const = delete;
    bool operator!=(const ClientID &rhs) const = delete;

} __attribute__((packed));
std::ostream &operator<<(std::ostream &os, const ClientID &cid);

struct BaseMessage
{
    enum RpcType type;
    ClientID cid;
    char others[0];
} __attribute__((packed));

class Lease;
struct BaseMessage;
struct RPC
{
    Lease *ret_lease{nullptr};
    BaseMessage *request{nullptr};
    size_t dir_id{0};
    RetCode ret_code{RC::kOk};
    char *buffer_addr{nullptr};  // for rpc_{read|write|cas}
};

}  // namespace patronus