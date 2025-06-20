#pragma once
#include <iostream>

#include "fmt/core.h"
#include "glog/logging.h"
#include "infiniband/verbs.h"
#include "infiniband/verbs_exp.h"
#include "util/Hexdump.hpp"
#include "util/UP.h"

struct PreExpAccess
{
    PreExpAccess(uint64_t flags) : f(flags)
    {
    }
    uint64_t f;
};

inline std::ostream &operator<<(std::ostream &os, const PreExpAccess &e)
{
    auto f = e.f;
    std::vector<const char *> outs;
    if (f & IBV_EXP_ACCESS_LOCAL_WRITE)
        outs.push_back("LOCAL_WRITE");
    if (f & IBV_EXP_ACCESS_REMOTE_WRITE)
        outs.push_back("REMOTE_WRITE");
    if (f & IBV_EXP_ACCESS_REMOTE_READ)
        outs.push_back("REMOTE_READ");
    if (f & IBV_EXP_ACCESS_REMOTE_ATOMIC)
        outs.push_back("REMOTE_ATOMIC");
    if (f & IBV_EXP_ACCESS_MW_BIND)
        outs.push_back("MW_BIND");
    if (f & IBV_EXP_ACCESS_ALLOCATE_MR)
        outs.push_back("ALLOCATE_MR");
    if (f & IBV_EXP_ACCESS_SHARED_MR_USER_READ)
        outs.push_back("SHARED_MR_UR");
    if (f & IBV_EXP_ACCESS_SHARED_MR_USER_WRITE)
        outs.push_back("SHARED_MR_UW");
    if (f & IBV_EXP_ACCESS_SHARED_MR_GROUP_READ)
        outs.push_back("SHARED_MR_GR");
    if (f & IBV_EXP_ACCESS_SHARED_MR_GROUP_WRITE)
        outs.push_back("SHARED_MR_GW");
    if (f & IBV_EXP_ACCESS_SHARED_MR_OTHER_READ)
        outs.push_back("SHARED_MR_OR");
    if (f & IBV_EXP_ACCESS_SHARED_MR_OTHER_WRITE)
        outs.push_back("SHARED_MR_OW");
    if (f & IBV_EXP_ACCESS_NO_RDMA)
        outs.push_back("NO_RDMA");
    if (f & IBV_EXP_ACCESS_MW_ZERO_BASED)
        outs.push_back("MW_ZERO_BASED");
    if (f & IBV_EXP_ACCESS_ON_DEMAND)
        outs.push_back("ON_DEMAND");
    if (f & IBV_EXP_ACCESS_RELAXED)
        outs.push_back("RELAXED");
    if (f & IBV_EXP_ACCESS_PHYSICAL_ADDR)
        outs.push_back("PHYSICAL_ADDR");
    if (f & IBV_EXP_ACCESS_TUNNELED_ATOMIC)
        outs.push_back("TUNNELED_ATOMIC");
    if (f & IBV_EXP_ACCESS_RELAXED_ORDERING)
        outs.push_back("RELAXED_ORDERING");
    if (f & IBV_EXP_ACCESS_RESERVED)
        outs.push_back("RESERVED");
    os << "exp_access: " << ::util::pre(outs);
    return os;
}

struct PreSendFlags
{
    PreSendFlags(uint64_t flags) : f(flags)
    {
    }
    uint64_t f;
};

inline std::ostream &operator<<(std::ostream &os, const PreSendFlags &fl)
{
    auto f = fl.f;
    std::vector<const char *> send_flags;
    if (f & IBV_SEND_FENCE)
        send_flags.push_back("FENSE");
    if (f & IBV_SEND_SIGNALED)
        send_flags.push_back("SIGNAL");
    if (f & IBV_SEND_SOLICITED)
        send_flags.push_back("SOLICITED");
    if (f & IBV_SEND_INLINE)
        send_flags.push_back("INLINE");
    if (f & IBV_EXP_SEND_IP_CSUM)
        send_flags.push_back("IP_CSUM");
    if (f & IBV_EXP_SEND_WITH_CALC)
        send_flags.push_back("CALC");
    if (f & IBV_EXP_SEND_WAIT_EN_LAST)
        send_flags.push_back("EN_LAST");
    if (f & IBV_EXP_SEND_EXT_ATOMIC_INLINE)
        send_flags.push_back("EXT_ATOMIC_INLINE");
    os << "send_flags: " << util::pre(send_flags);
    return os;
}

inline std::ostream &operator<<(std::ostream &os, ibv_wc_opcode op)
{
    switch (op)
    {
    case IBV_WC_SEND:
        os << "IBV_WC_SEND";
        break;
    case IBV_WC_RDMA_WRITE:
        os << "IBV_WC_RDMA_WRITE";
        break;
    case IBV_WC_RDMA_READ:
        os << "IBV_WC_RDMA_READ";
        break;
    case IBV_WC_COMP_SWAP:
        os << "IBV_WC_COMP_SWAP";
        break;
    case IBV_WC_FETCH_ADD:
        os << "IBV_WC_FETCH_ADD";
        break;
    case IBV_WC_BIND_MW:
        os << "IBV_WC_BIND_MW";
        break;
    case IBV_WC_LOCAL_INV:
        os << "IBV_WC_LOCAL_INV";
        break;
    case IBV_WC_RECV:
        os << "IBV_WC_RECV";
        break;
    case IBV_WC_RECV_RDMA_WITH_IMM:
        os << "IBV_WC_RECV_RDMA_WITH_IMM";
        break;
    default:
        os << "UNKNOWN(" << (int) op << ")";
        break;
    }
    return os;
}
inline const char *wc_opcode_str(ibv_wc_opcode op)
{
    switch (op)
    {
    case IBV_WC_SEND:
        return "IBV_WC_SEND";
    case IBV_WC_RDMA_WRITE:
        return "IBV_WC_RDMA_WRITE";
    case IBV_WC_RDMA_READ:
        return "IBV_WC_RDMA_READ";
    case IBV_WC_COMP_SWAP:
        return "IBV_WC_COMP_SWAP";
    case IBV_WC_FETCH_ADD:
        return "IBV_WC_FETCH_ADD";
    case IBV_WC_BIND_MW:
        return "IBV_WC_BIND_MW";
    case IBV_WC_LOCAL_INV:
        return "IBV_WC_LOCAL_INV";
    case IBV_WC_RECV:
        return "IBV_WC_RECV";
    case IBV_WC_RECV_RDMA_WITH_IMM:
        return "IBV_WC_RECV_RDMA_WITH_IMM";
    default:
        LOG(WARNING) << "Unknown wc_opcode(" << (int) op << ")";
        return "UNKNOWN";
    }
}
enum class wr_union_type
{
    RDMA,
    ATOMIC,
    UD,
    UMR,
    ENABLE,
    WAIT,
    ExtCAS,
    ExtFAA,
};
inline const char *wr_opcode_str(ibv_wr_opcode op)
{
    switch (op)
    {
    case IBV_WR_RDMA_WRITE:
        return "IBV_WR_RDMA_WRITE";
    case IBV_WR_RDMA_WRITE_WITH_IMM:
        return "IBV_WR_RDMA_WRITE_WITH_IMM";
    case IBV_WR_SEND:
        return "IBV_WR_SEND";
    case IBV_WR_SEND_WITH_IMM:
        return "IBV_WR_SEND_WITH_IMM";
    case IBV_WR_RDMA_READ:
        return "IBV_WR_RDMA_READ";
    case IBV_WR_ATOMIC_CMP_AND_SWP:
        return "IBV_WR_ATOMIC_CMP_AND_SWP";
    case IBV_WR_ATOMIC_FETCH_AND_ADD:
        return "IBV_WR_ATOMIC_FETCH_AND_ADD";
    case IBV_WR_LOCAL_INV:
        return "IBV_WR_LOCAL_INV";
    case IBV_WR_BIND_MW:
        return "IBV_WR_BIND_MW";
    case IBV_WR_SEND_WITH_INV:
        return "IBV_WR_SEND_WITH_INV";
    default:
        LOG(WARNING) << "Unknown wr_opcode(" << int(op) << ")";
        return "UNKNOWN";
    }
}
inline const char *wr_exp_opcode_str(ibv_exp_wr_opcode op)
{
    switch (op)
    {
    case IBV_EXP_WR_TSO:
        return "IBV_EXP_WR_TSO";
    case IBV_EXP_WR_SEND_ENABLE:
        return "IBV_EXP_WR_SEND_ENABLE";
    case IBV_EXP_WR_RECV_ENABLE:
        return "IBV_EXP_WR_RECV_ENABLE";
    case IBV_EXP_WR_CQE_WAIT:
        return "IBV_EXP_WR_CQE_WAIT";
    case IBV_EXP_WR_EXT_MASKED_ATOMIC_CMP_AND_SWP:
        return "IBV_EXP_WR_EXT_MASKED_ATOMIC_CMP_AND_SWP";
    case IBV_EXP_WR_EXT_MASKED_ATOMIC_FETCH_AND_ADD:
        return "IBV_EXP_WR_EXT_MASKED_ATOMIC_FETCH_AND_ADD";
    case IBV_EXP_WR_NOP:
        return "IBV_EXP_WR_NOP";
    case IBV_EXP_WR_UMR_FILL:
        return "IBV_EXP_WR_UMR_FILL";
    case IBV_EXP_WR_UMR_INVALIDATE:
        return "IBV_EXP_WR_UMR_INVALIDATE";
    default:
        return wr_opcode_str((ibv_wr_opcode) op);
    }
}
inline constexpr wr_union_type wr_wr_union_type(ibv_wr_opcode op)
{
    switch (op)
    {
    case IBV_WR_RDMA_WRITE:
    case IBV_WR_RDMA_WRITE_WITH_IMM:
    case IBV_WR_RDMA_READ:
        return wr_union_type::RDMA;
    // this may not correct:
    // posting IBV_WR_SEND to RC QP have empty UD fields.
    case IBV_WR_SEND:
    case IBV_WR_SEND_WITH_IMM:
        return wr_union_type::UD;
    case IBV_WR_ATOMIC_CMP_AND_SWP:
    case IBV_WR_ATOMIC_FETCH_AND_ADD:
        return wr_union_type::ATOMIC;
    case IBV_WR_LOCAL_INV:
    case IBV_WR_BIND_MW:
    case IBV_WR_SEND_WITH_INV:
        return wr_union_type::UMR;
    }
}
inline constexpr wr_union_type wr_exp_wr_union_type(ibv_exp_wr_opcode op)
{
    switch (op)
    {
    case IBV_EXP_WR_SEND_ENABLE:
    case IBV_EXP_WR_RECV_ENABLE:
        return wr_union_type::ENABLE;
    case IBV_EXP_WR_CQE_WAIT:
        return wr_union_type::WAIT;
    case IBV_EXP_WR_UMR_FILL:
    case IBV_EXP_WR_UMR_INVALIDATE:
        return wr_union_type::UMR;
    case IBV_EXP_WR_EXT_MASKED_ATOMIC_CMP_AND_SWP:
        return wr_union_type::ExtCAS;
    case IBV_EXP_WR_EXT_MASKED_ATOMIC_FETCH_AND_ADD:
        return wr_union_type::ExtFAA;
    default:
        return wr_wr_union_type((ibv_wr_opcode) op);
    }
}
inline constexpr bool with_imm(ibv_wr_opcode op)
{
    return op == IBV_WR_RDMA_WRITE_WITH_IMM;
}

inline const char *exp_wc_opcode_str(ibv_exp_wc_opcode op)
{
    switch (op)
    {
    case IBV_EXP_WC_SEND:
        return "IBV_EXP_WC_SEND";
    case IBV_EXP_WC_RDMA_WRITE:
        return "IBV_EXP_WC_RDMA_WRITE";
    case IBV_EXP_WC_RDMA_READ:
        return "IBV_EXP_WC_RDMA_READ";
    case IBV_EXP_WC_COMP_SWAP:
        return "IBV_EXP_WC_COMP_SWAP";
    case IBV_EXP_WC_FETCH_ADD:
        return "IBV_EXP_WC_FETCH_ADD";
    case IBV_EXP_WC_BIND_MW:
        return "IBV_EXP_WC_BIND_MW";
    case IBV_EXP_WC_LOCAL_INV:
        return "IBV_EXP_WC_LOCAL_INV";
    case IBV_EXP_WC_MASKED_COMP_SWAP:
        return "IBV_EXP_WC_MASKED_COMP_SWAP";
    case IBV_EXP_WC_MASKED_FETCH_ADD:
        return "IBV_EXP_WC_MASKED_FETCH_ADD";
    case IBV_EXP_WC_TSO:
        return "IBV_EXP_WC_TSO";
    case IBV_EXP_WC_UMR:
        return "IBV_EXP_WC_UMR";
    case IBV_EXP_WC_RECV:
        return "IBV_EXP_WC_RECV";
    case IBV_EXP_WC_RECV_RDMA_WITH_IMM:
        return "IBV_EXP_WC_RECV_RDMA_WITH_IMM";
    case IBV_EXP_WC_TM_ADD:
        return "IBV_EXP_WC_TM_ADD";
    case IBV_EXP_WC_TM_DEL:
        return "IBV_EXP_WC_TM_DEL";
    case IBV_EXP_WC_TM_SYNC:
        return "IBV_EXP_WC_TM_SYNC";
    case IBV_EXP_WC_TM_RECV:
        return "IBV_EXP_WC_TM_RECV";
    case IBV_EXP_WC_TM_NO_TAG:
        return "IBV_EXP_WC_TM_NO_TAG";
    case IBV_EXP_WC_RECV_NOP:
        return "IBV_EXP_WC_RECV_NOP";
    default:
        return "Unknown";
    }
}

inline std::ostream &operator<<(std::ostream &os, const ibv_wc &wc)
{
    bool has_imm = wc.wc_flags & IBV_WC_WITH_IMM;
    os << "{WC wr_id: " << wc.wr_id
       << ", status: " << ibv_wc_status_str(wc.status)
       << ", opcode: " << wc.opcode;
    os << ", byte_len: " << wc.byte_len;
    if (wc.status != IBV_WC_SUCCESS)
    {
        os << ", vendor_err: " << wc.vendor_err;
    }
    if (has_imm)
    {
        os << ", imm_data: " << wc.imm_data;
    }
    os << ", qp_num: " << wc.qp_num << ", src_qp: " << wc.src_qp;
    os << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const ibv_exp_wc &wc)
{
    os << "{WC wr_id: " << (void *) wc.wr_id
       << ", status: " << ibv_wc_status_str(wc.status)
       << ", opcode: " << exp_wc_opcode_str(wc.exp_opcode);
    os << ", byte_len: " << wc.byte_len;
    if (wc.status != IBV_WC_SUCCESS)
    {
        os << ", vendor_err: " << wc.vendor_err;
    }
    if (wc.exp_wc_flags & IBV_EXP_WC_WITH_IMM)
    {
        os << ", imm_data: " << wc.imm_data;
    }
    os << ", qp_num: " << wc.qp_num << ", src_qp: " << wc.src_qp;
    os << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const ibv_mw_bind_info &info)
{
    os << "{bind_info mr: " << (void *) info.mr
       << ", addr: " << (void *) info.addr << ", length: " << info.length
       << ", access_flags: " << PreExpAccess(info.mw_access_flags);
    os << "}";
    return os;
}
inline std::ostream &operator<<(std::ostream &os,
                                const ibv_exp_mw_bind_info &info)
{
    os << "{exp_bind_info mr: " << (void *) info.mr
       << ", addr: " << (void *) info.addr << ", length: " << info.length
       << ", exp_access_flags: " << PreExpAccess(info.exp_mw_access_flags);
    os << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const ibv_sge &sge)
{
    os << "(" << (void *) sge.addr << ", " << sge.length
       << ", lkey: " << sge.lkey << ")";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const ibv_send_wr &wr)
{
    os << "{WR wr_id: " << wr.wr_id;
    if (wr.next)
    {
        os << ", next: " << (void *) wr.next;
    }
    if (wr.num_sge)
    {
        os << ", sg_list: [";
        for (int i = 0; i < wr.num_sge; ++i)
        {
            os << wr.sg_list[i];
            if (i + 1 != wr.num_sge)
            {
                os << ", ";
            }
        }
        os << "]";
    }
    os << ", opcode: " << wr_opcode_str(wr.opcode);

    os << ", " << PreSendFlags(wr.send_flags);

    if (with_imm(wr.opcode))
    {
        os << ", imm_data: " << wr.imm_data;
    }
    switch (wr_wr_union_type(wr.opcode))
    {
    case wr_union_type::RDMA:
    {
        os << ", remote_addr: " << (void *) wr.wr.rdma.remote_addr;
        os << ", rkey: " << wr.wr.rdma.rkey;
        break;
    }
    case wr_union_type::ATOMIC:
    {
        os << ", remote_addr: " << (void *) wr.wr.atomic.remote_addr;
        os << ", compare_add: " << wr.wr.atomic.compare_add;
        os << ", swap: " << wr.wr.atomic.swap;
        os << ", rkey: " << wr.wr.atomic.rkey;
        break;
    }
    case wr_union_type::UD:
    {
        os << ", remote_qpn: " << wr.wr.ud.remote_qpn;
        os << ", remote_qkey: " << wr.wr.ud.remote_qkey;
        break;
    }
    case wr_union_type::UMR:
    {
        os << ", mw: " << (void *) wr.bind_mw.mw;
        os << ", rkey: " << wr.bind_mw.rkey;
        os << ", bind_info: " << wr.bind_mw.bind_info;
        break;
    }
    case wr_union_type::ENABLE:
    case wr_union_type::WAIT:
    {
        LOG(FATAL) << "Unexpected wr_union_type: " << wr.opcode;
        break;
    }
    default:
    {
        os << ", TODO...";
    }
    }
    os << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const ibv_recv_wr &wr)
{
    os << "{ibv_recv_wr wr_id: " << wr.wr_id << ", next: " << wr.next;
    if (wr.num_sge)
    {
        os << ", sg_list: [";
        for (int i = 0; i < wr.num_sge; ++i)
        {
            os << wr.sg_list[i];
            if (i + 1 != wr.num_sge)
            {
                os << ", ";
            }
        }
        os << "]";
    }
    os << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os,
                                const ibv_exp_mkey_list_container &c)
{
    os << "{mkey_list_container max_lkm_list_size: " << c.max_klm_list_size
       << "}";
    return os;
}
inline std::ostream &operator<<(std::ostream &os, const ibv_exp_mem_region &r)
{
    os << "{base_addr: " << (void *) r.base_addr << ", mr: " << r.mr
       << ", length: " << r.length << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os,
                                const ibv_exp_mem_repeat_block &b)
{
    os << "{repeat_block " << (void *) b.base_addr << ", mr: " << b.mr
       << ", byte_count: " << b.byte_count[0] << ", stride: " << b.stride[0]
       << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const ibv_exp_send_wr &wr)
{
    os << "{ibv_exp_send_wr wr_id: " << wr.wr_id;
    if (wr.next)
    {
        os << ", next: " << (void *) wr.next;
    }
    if (wr.num_sge)
    {
        os << ", sg_list: [";
        for (int i = 0; i < wr.num_sge; ++i)
        {
            os << wr.sg_list[i];
            if (i + 1 != wr.num_sge)
            {
                os << ", ";
            }
        }
        os << "]";
    }
    os << ", opcode: " << wr_exp_opcode_str(wr.exp_opcode);

    os << ", " << PreSendFlags(wr.exp_send_flags);

    if (with_imm((ibv_wr_opcode) wr.exp_opcode))
    {
        os << ", imm_data: " << wr.ex.imm_data;
    }
    switch (wr_exp_wr_union_type(wr.exp_opcode))
    {
    case wr_union_type::RDMA:
    {
        os << ", remote_addr: " << (void *) wr.wr.rdma.remote_addr;
        os << ", rkey: " << wr.wr.rdma.rkey;
        break;
    }
    case wr_union_type::ATOMIC:
    {
        os << ", remote_addr: " << (void *) wr.wr.atomic.remote_addr;
        os << ", compare_add: " << wr.wr.atomic.compare_add;
        os << ", swap: " << wr.wr.atomic.swap;
        os << ", rkey: " << wr.wr.atomic.rkey;
        break;
    }
    case wr_union_type::ExtCAS:
    {
        auto &ma = wr.ext_op.masked_atomics;
        os << ", log_arg_sz: " << ma.log_arg_sz;
        os << ", remote_addr: " << (void *) ma.remote_addr;
        os << ", rkey: " << ma.rkey;
        auto &op = ma.wr_data.inline_data.op.cmp_swap;
        // size > 8, not inlined
        auto size = 1 << ma.log_arg_sz;
        if (size > 8)
        {
            os << ", compare_val: "
               << util::InlinedHexdump((void *) op.compare_val, size);
            os << ", swap_val: "
               << util::InlinedHexdump((void *) op.swap_val, size);
            os << ", compare_mask: "
               << util::InlinedHexdump((void *) op.compare_mask, size);
            os << ", swap_mask: "
               << util::InlinedHexdump((void *) op.swap_mask, size);
        }
        else
        {
            os << ", compare_val: " << (void *) op.compare_val;
            os << ", swap_val: " << (void *) op.swap_val;
            os << ", compare_mask: " << (void *) op.compare_mask;
            os << ", swap_mask: " << (void *) op.swap_mask;
        }
        break;
    }
    case wr_union_type::ExtFAA:
    {
        auto &ma = wr.ext_op.masked_atomics;
        os << ", log_arg_sz: " << ma.log_arg_sz;
        os << ", remote_addr: " << (void *) ma.remote_addr;
        os << ", rkey: " << ma.rkey;
        auto &op = ma.wr_data.inline_data.op.fetch_add;
        auto size = 1 << ma.log_arg_sz;
        if (size > 8)
        {
            os << ", add_val: "
               << util::InlinedHexdump((void *) op.add_val, size);
            os << ", field_boundary: "
               << util::InlinedHexdump((void *) op.field_boundary, size);
        }
        else
        {
            os << ", add_val: " << (void *) op.add_val;
            os << ", field_boundary: " << (void *) op.field_boundary;
        }
        break;
    }
    case wr_union_type::UD:
    {
        os << ", remote_qpn: " << wr.wr.ud.remote_qpn;
        os << ", remote_qkey: " << wr.wr.ud.remote_qkey;
        break;
    }
    case wr_union_type::UMR:
    {
        auto &umr = wr.ext_op.umr;

        if (umr.memory_objects)
        {
            os << "mkey_list: " << umr.memory_objects;
        }
        os << ", base_addr: " << (void *) umr.base_addr;
        os << ", num_mrs: " << umr.num_mrs;
        os << ", modified_mr: " << (void *) umr.modified_mr;
        os << ", exp_access: " << PreExpAccess(umr.exp_access);

        switch (umr.umr_type)
        {
        case IBV_EXP_UMR_MR_LIST:
        {
            auto &mem_reg_list = umr.mem_list.mem_reg_list;
            os << ", mem_reg_list: [";
            for (size_t i = 0; i < umr.num_mrs; ++i)
            {
                os << mem_reg_list[i];
                bool last = i + 1 == umr.num_mrs;
                if (!last)
                {
                    os << ", ";
                }
            }
            os << "]";

            break;
        }
        case IBV_EXP_UMR_REPEAT:
        {
            auto &rb = umr.mem_list.rb;
            os << ", repeat_block_list: ";
            if (rb.mem_repeat_block_list)
            {
                os << *rb.mem_repeat_block_list;
            }
            else
            {
                os << "nullptr";
            }
            os << ", repeat_count: [";
            for (size_t d = 0; d < rb.stride_dim; ++d)
            {
                os << rb.repeat_count[d];
                bool last = d + 1 == rb.stride_dim;
                if (!last)
                {
                    os << ", ";
                }
            }
            os << "]";
            os << ", stride_dim: " << rb.stride_dim;
            break;
        }
        case IBV_EXP_UMR_MR_LIST_FIXED_SIZE:
        {
            os << ", [TODO...]";
            break;
        }
        default:
            LOG(FATAL) << "Unknown umr_type " << (int) umr.umr_type;
        }
        break;
    }
    case wr_union_type::ENABLE:
    {
        os << fmt::format(", {{ENABLE qp: {}, wqe_count: {}}}",
                          (void *) wr.task.wqe_enable.qp,
                          wr.task.wqe_enable.wqe_count);
        break;
    }
    case wr_union_type::WAIT:
    {
        os << fmt::format(", {{WAIT cq: {}, cq_count: {}}}",
                          (void *) wr.task.cqe_wait.cq,
                          wr.task.cqe_wait.cq_count);
        break;
    }
    default:
    {
        os << ", TODO... ";
    }
    }
    os << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const ibv_mr &mr)
{
    os << "{ibv_mr addr: " << (void *) mr.addr << ", length: " << mr.length
       << ", lkey: " << mr.lkey << ", rkey: " << mr.rkey << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const ibv_qp_cap &cap)
{
    os << "{ibv_qp_cap send_wr: " << cap.max_send_wr;
    os << ", recv_wr: " << cap.max_recv_wr;
    os << ", send_sge: " << cap.max_send_sge;
    os << ", recv_sge: " << cap.max_recv_sge;
    os << ", inline_data: " << cap.max_inline_data;
    os << "}";
    return os;
}
inline std::ostream &operator<<(std::ostream &os, const ibv_exp_qpg &g)
{
    os << "{ibv_exp_qpg ";
    os << "qpg_type: " << g.qpg_type;
    os << "}";
    return os;
}

inline std::ostream &operator<<(std::ostream &os, ibv_qp_type t)
{
    switch (t)
    {
    case IBV_QPT_RC:
        os << "RC";
        break;
    case IBV_QPT_UD:
        os << "UD";
        break;
    case IBV_QPT_XRC:
        os << "XRC";
        break;
    case IBV_QPT_RAW_PACKET:
        os << "RAW_PACKET(ETH)";
        break;
    case IBV_QPT_XRC_SEND:
        os << "XRC_SEND";
        break;
    case IBV_QPT_XRC_RECV:
        os << "XRC_RECV";
        break;
    default:
        os << "Unknown(" << (int) t << ")";
        break;
    }
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const ibv_exp_qp_init_attr &a)
{
    os << "{ibv_exp_qp_init_attr ";
    os << "qp_context: " << (void *) a.qp_context;
    os << ", send_cq: " << a.send_cq;
    os << ", recv_cq: " << a.recv_cq << ", srq: " << a.srq << ", " << a.cap
       << ", type: " << a.qp_type << ", sq_sig_all: " << a.sq_sig_all;
    os << ", comp_mask: [";
    auto m = a.comp_mask;
    if (m & IBV_EXP_QP_INIT_ATTR_PD)
    {
        os << "PD ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_XRCD)
    {
        os << "XRCD ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS)
    {
        os << "FLAGS ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_INL_RECV)
    {
        os << "InlRecv ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_QPG)
    {
        os << "QPG ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_ATOMICS_ARG)
    {
        os << "AtmArg ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_MAX_INL_KLMS)
    {
        os << "KLMS ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_RES_DOMAIN)
    {
        os << "Res ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_RX_HASH)
    {
        os << "Hash ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_PORT)
    {
        os << "Port ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_PEER_DIRECT)
    {
        os << "PeerDirect ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_MAX_TSO_HEADER)
    {
        os << "TSO ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_ASSOCIATED_QPN)
    {
        os << "QPN ";
    }
    if (m & IBV_EXP_QP_INIT_ATTR_RES_DOMAIN)
    {
        os << "Res ";
    }
    os << "] (" << (void *) (uint64_t) m << ")";
    os << ", pd: " << a.pd;
    os << ", xrcd: " << a.xrcd;

    os << ", exp_create_flags: [";
    auto f = a.exp_create_flags;
    if (f & IBV_EXP_QP_CREATE_CROSS_CHANNEL)
    {
        os << "XChnl ";
    }
    if (f & IBV_EXP_QP_CREATE_MANAGED_SEND)
    {
        os << "MngSnd ";
    }
    if (f & IBV_EXP_QP_CREATE_MANAGED_RECV)
    {
        os << "MngRecv ";
    }
    if (f & IBV_EXP_QP_CREATE_IGNORE_SQ_OVERFLOW)
    {
        os << "IgnSq ";
    }
    if (f & IBV_EXP_QP_CREATE_IGNORE_RQ_OVERFLOW)
    {
        os << "IgnRq ";
    }
    if (f & IBV_EXP_QP_CREATE_ATOMIC_BE_REPLY)
    {
        os << "AtmBE ";
    }
    if (f & IBV_EXP_QP_CREATE_UMR)
    {
        os << "UMR ";
    }
    if (f & IBV_EXP_QP_CREATE_EC_PARITY_EN)
    {
        os << "EC ";
    }
    if (f & IBV_EXP_QP_CREATE_RX_END_PADDING)
    {
        os << "EndPadding ";
    }
    if (f & IBV_EXP_QP_CREATE_SCATTER_FCS)
    {
        os << "SctFCS ";
    }
    if (f & IBV_EXP_QP_CREATE_TUNNEL_OFFLOADS)
    {
        os << "TnlOfld ";
    }
    if (f & IBV_EXP_QP_CREATE_INTERNAL_USE)
    {
        os << "IntnlUse ";
    }
    if (f & IBV_EXP_QP_CREATE_PACKET_BASED_CREDIT_MODE)
    {
        os << "Credit ";
    }
    os << "] (" << (void *) (uint64_t) f << ")";
    os << ", max_inl_recv: " << a.max_inl_recv;
    os << ", qpg: " << a.qpg;
    os << ", max_atomic_arg: " << a.max_atomic_arg;
    os << ", max_inl_send_klms: " << a.max_inl_send_klms;
    os << ", res_domain: " << a.res_domain;
    os << ", rx_hash_conf: " << a.rx_hash_conf;
    os << ", port_num: " << (int) a.port_num;
    os << ", peer_direct_attrs: " << a.peer_direct_attrs;
    os << ", max_tso_header: " << a.max_tso_header;
    os << ", associated_qpn: " << a.associated_qpn;
    os << "}";
    return os;
}

struct PreWRLink
{
    PreWRLink(ibv_exp_send_wr *wr) : exp_wr_(wr)
    {
    }
    PreWRLink(ibv_send_wr *wr) : wr_(wr)
    {
    }
    ibv_exp_send_wr *exp_wr_{};
    ibv_send_wr *wr_{};
};

inline std::ostream &operator<<(std::ostream &os, const PreWRLink &l)
{
    auto *cur_exp_wr = l.exp_wr_;
    while (cur_exp_wr)
    {
        os << *cur_exp_wr << std::endl;
        cur_exp_wr = cur_exp_wr->next;
    }
    auto *cur_wr = l.wr_;
    while (cur_wr)
    {
        os << *cur_wr << std::endl;
        cur_wr = cur_wr->next;
    }
    return os;
}