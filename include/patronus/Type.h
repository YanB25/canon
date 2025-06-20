#pragma once

#ifndef PATRONUS_TYPE_H_
#define PATRONUS_TYPE_H_

#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Common.h"
#include "patronus/BasicType.h"
#include "patronus/Config.h"
#include "patronus/Time.h"
#include "umsg/Config.h"
#include "umsg/UnreliableConnection.h"
#include "util/Coro.h"
#include "util/Debug.h"

namespace patronus
{
enum class AcquireRequestStatus : uint8_t
{
    kSuccess,
    kMagicMwErr,
    kLockedErr,
    kBindErr,
    kRegMrErr,
    kAddressOutOfRangeErr,
    kNoMem,
    kNoMw,
    kReserved,
    kReservedNoReturn,
};
std::ostream &operator<<(std::ostream &os, AcquireRequestStatus status);

enum class AcquireRequestFlag : uint16_t
{
    kNoGc = 1 << 0,
    kWithConflictDetect = 1 << 1,
    kNoBindPR = 1 << 2,
    kNoBindAny = 1 << 3,
    kWithAllocation = 1 << 4,
    kOnlyAllocation = 1 << 5,
    kUseMR = 1 << 6,
    kNoRpc = 1 << 7,
    kDebugServerDoNothing = 1 << 8,
    kDebugFlag_1 = 1 << 9,
    kReserved = 1 << 10,
};

void debug_validate_acquire_request_flag(flag_t flag);
struct AcquireRequestFlagOut
{
    AcquireRequestFlagOut(flag_t flag) : flag(flag)
    {
    }
    flag_t flag;
};
std::ostream &operator<<(std::ostream &os, AcquireRequestFlagOut flag);
struct AcquireRequest
{
    enum RpcType type;
    ClientID cid;
    id_t key;
    size_t size;
    time::ns_t required_ns;
    uint16_t dir_id;
    trace_t trace;
    uint16_t flag;  // should be AcquireRequestFlag
    Debug<uint64_t> digest;
} __attribute__((packed));
static_assert(sizeof(AcquireRequest) < ::config::umsg::kUserMessageSize);
static_assert(NR_DIRECTORY <
              std::numeric_limits<decltype(AcquireRequest::dir_id)>::max());
static_assert(sizeof(AcquireRequest::flag) >= sizeof(AcquireRequestFlag));
std::ostream &operator<<(std::ostream &os, const AcquireRequest &req);

struct AcquireResponse
{
    enum RpcType type;
    ClientID cid;
    uint32_t rkey_0;
    uint32_t rkey_header;
    uint64_t buffer_base;
    uint64_t header_base;
    // about time management of Lease
    time::term_t begin_term;
    uint32_t ns_per_unit;
    uint16_t lease_id;
    uint32_t aba_id;
    uint32_t post_qp_id;
    AcquireRequestStatus status;
} __attribute__((packed));
static_assert(sizeof(AcquireResponse) < ::config::umsg::kUserMessageSize);
std::ostream &operator<<(std::ostream &os, const AcquireResponse &resp);

enum class AdminFlag : uint8_t
{
    kAdminReqExit = 0,
    kAdminReqRecovery = 1,
    kAdminBarrier = 2,
    kAdminQPtoRO = 3,
    kAdminQPtoRW = 4,
};
std::ostream &operator<<(std::ostream &os, const AdminFlag &f);

struct AdminRequest
{
    enum RpcType type;
    ClientID cid;
    uint8_t flag;  // enum AdminFlag
    Debug<uint64_t> digest;
    uint16_t dir_id;
    uint64_t data;  // used by p->barrier()
    bool need_response;
} __attribute__((packed));
static_assert(sizeof(AdminRequest) < ::config::umsg::kUserMessageSize);
static_assert(sizeof(AdminRequest::flag) >= sizeof(AdminFlag));
std::ostream &operator<<(std::ostream &os, const AdminRequest &resp);

struct AdminResponse
{
    enum RpcType type;
    ClientID cid;
    uint8_t flag;  // enum AdminFlag
    bool success;
    Debug<uint64_t> digest;
} __attribute__((packed));
static_assert(sizeof(AdminResponse) < ::config::umsg::kUserMessageSize);
std::ostream &operator<<(std::ostream &os, const AdminResponse &resp);

enum class LeaseModifyFlag : uint16_t
{
    kNoRelinquishUnbindAny = 1 << 0,
    kNoRelinquishUnbindPr = 1 << 1,
    kForceUnbind = 1 << 2,
    kWithDeallocation = 1 << 3,
    kOnlyDeallocation = 1 << 4,
    kServerDoNothing = 1 << 5,
    // wait until unbind success before returning
    // will harm performance
    kWaitUntilSuccess = 1 << 6,
    kUseMR = 1 << 7,
    kNoRpc = 1 << 8,
    kDebugExtendDoNothing = 1 << 9,
    kReserved = 1 << 10,
};
void debug_validate_lease_modify_flag(flag_t flag);
struct LeaseModifyFlagOut
{
    LeaseModifyFlagOut(flag_t flag) : flag(flag)
    {
    }
    flag_t flag;
};
std::ostream &operator<<(std::ostream &os, LeaseModifyFlagOut flag);

struct LeaseModifyRequest
{
    enum RpcType type;
    ClientID cid;
    id_t lease_id;
    time::ns_t ns;
    uint64_t hint; /* when only_dealloc is ON */
    uint16_t flag; /* LeaseModifyFlag */
    uint64_t addr; /* when only_dealloc is ON */
    uint32_t size; /* when only_dealloc is ON */
    Debug<uint64_t> digest;
} __attribute__((packed));
static_assert(sizeof(LeaseModifyRequest) <= ::config::umsg::kUserMessageSize);
std::ostream &operator<<(std::ostream &os, const LeaseModifyRequest &req);
static_assert(sizeof(LeaseModifyRequest::flag) >= sizeof(LeaseModifyFlag));

struct LeaseModifyResponse
{
    enum RpcType type;
    ClientID cid;
    bool success;
    Debug<uint64_t> digest;

} __attribute__((packed));
static_assert(sizeof(LeaseModifyResponse) < ::config::umsg::kUserMessageSize);
std::ostream &operator<<(std::ostream &os, const LeaseModifyResponse &req);

struct BatchBaseMessage
{
    enum RpcType type;
    ClientID cid;
    char others[64 - sizeof(type) - sizeof(cid)];
} __attribute__((packed));
static_assert(sizeof(BatchBaseMessage) == 64);

enum class RWFlag : uint8_t
{
    kNoLocalExpireCheck = 1 << 0,
    kWithAutoExtend = 1 << 1,
    kWithCache = 1 << 2,
    kUseUniversalRkey = 1 << 3,
    kEnableTrace = 1 << 4,
    kUseTwoSided = 1 << 5,
    kUseTwoSidedAutoPacking = 1 << 6,
    kReserved = 1 << 7,
};

struct RWFlagOut
{
    RWFlagOut(flag_t flag) : flag(flag)
    {
    }
    flag_t flag;
};
std::ostream &operator<<(std::ostream &os, RWFlagOut flag);

enum class MemoryRequestFlag : uint8_t
{
    kRead,
    kWrite,
    kCAS,
    kFAA
};
std::ostream &operator<<(std::ostream &os, MemoryRequestFlag flag);
struct MemoryRequest
{
    enum RpcType type;
    ClientID cid;
    uint8_t flag;  // MemoryRequestFlag
    uint64_t remote_addr;
    Debug<uint64_t> digest;
    uint16_t size;
    char buffer[];
    size_t msg_size() const
    {
        return sizeof(MemoryRequest) + size;
    }
    bool validate() const
    {
        return msg_size() <= ::config::umsg::kUserMessageSize;
    }
    static constexpr size_t buffer_capacity()
    {
        return ::config::umsg::kUserMessageSize - sizeof(MemoryRequest);
    }
    static size_t msg_expect_size(size_t buffer_size)
    {
        return sizeof(MemoryRequest) + buffer_size;
    }
} __attribute__((packed));
static_assert(sizeof(MemoryRequest::flag) >= sizeof(MemoryRequestFlag));
static_assert(std::numeric_limits<decltype(MemoryRequest::size)>::max() >= 32);
std::ostream &operator<<(std::ostream &os, const MemoryRequest &req);

struct MemcpyRequest
{
    enum RpcType type;
    ClientID cid;
    // uint64_t source;
    // uint64_t target;
    uint32_t times;
    uint32_t size;
    bool offload;
    Debug<uint64_t> digest;
} __attribute__((packed));
static_assert(sizeof(MemcpyRequest) < ::config::umsg::kUserMessageSize);
std::ostream &operator<<(std::ostream &os, const MemcpyRequest &req);

struct MemcpyResponse
{
    enum RpcType type;
    ClientID cid;
    Debug<uint64_t> digest;
};
static_assert(sizeof(MemcpyResponse) < ::config::umsg::kUserMessageSize);
std::ostream &operator<<(std::ostream &os, const MemcpyResponse &);

struct MemoryResponse
{
    enum RpcType type;
    ClientID cid;
    uint8_t flag;  // MemoryRequestFlag
    bool success;
    size_t size;
    Debug<uint64_t> digest;
    char buffer[];
    size_t msg_size() const
    {
        return sizeof(MemoryResponse) + size;
    }
    bool validate()
    {
        return msg_size() <= ::config::umsg::kUserMessageSize;
    }
    static constexpr size_t buffer_capacity()
    {
        return ::config::umsg::kUserMessageSize - sizeof(MemoryResponse);
    }
    static size_t msg_expect_size(size_t buffer_size)
    {
        return sizeof(MemoryResponse) + buffer_size;
    }
} __attribute__((packed));
static_assert(sizeof(MemoryResponse) <= ::config::umsg::kUserMessageSize);
std::ostream &operator<<(std::ostream &os, const MemoryResponse &resp);

constexpr inline size_t MemoryMessagePayload()
{
    return std::min(MemoryRequest::buffer_capacity(),
                    MemoryResponse::buffer_capacity());
}

inline std::ostream &operator<<(std::ostream &os, const BaseMessage &msg)
{
    auto type = msg.type;
    switch (type)
    {
    case RpcType::kAcquireRLeaseReq:
    case RpcType::kAcquireWLeaseReq:
    case RpcType::kAcquireNoLeaseReq:
        os << *(AcquireRequest *) &msg;
        break;
    case RpcType::kAcquireLeaseResp:
        os << *(AcquireResponse *) &msg;
        break;
    case RpcType::kRelinquishReq:
    case RpcType::kExtendReq:
        os << *(LeaseModifyRequest *) &msg;
        break;
    case RpcType::kRelinquishResp:
    case RpcType::kExtendResp:
        os << *(LeaseModifyResponse *) &msg;
        break;
    case RpcType::kAdminReq:
    case RpcType::kAdmin:
        os << *(AdminRequest *) &msg;
        break;
    case RpcType::kAdminResp:
        os << *(AdminResponse *) &msg;
        break;
    case RpcType::kMemoryReq:
        os << *(MemoryRequest *) &msg;
        break;
    case RpcType::kMemoryResp:
        os << *(MemoryResponse *) &msg;
        break;
    case RpcType::kMemcpyReq:
        os << *(MemcpyRequest *) &msg;
        break;
    case RpcType::kMemcpyResp:
        os << *(MemcpyResponse *) &msg;
        break;
    default:
        os << "{Unknown msg(" << type << ") cid: " << msg.cid << "... }";
    }
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const RPC &r)
{
    os << "{RPC ret_lease: " << (void *) r.ret_lease << ", dir_id: " << r.dir_id
       << ", ret_code: " << r.ret_code
       << ", buffer_addr: " << (void *) r.buffer_addr
       << ", request: " << *r.request;
    return os;
}

}  // namespace patronus

#endif