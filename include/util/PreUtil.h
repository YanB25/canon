#pragma once
#include <infiniband/verbs.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>

#include "Literals.h"
#include "util/PerformanceReporter.h"
#include "util/UP.h"

using namespace util::literals;

namespace util
{
class pre_ns
{
public:
    pre_ns(uint64_t ns) : ns_(ns)
    {
    }

    uint64_t ns_;
};
inline std::ostream &operator<<(std::ostream &os, pre_ns pns)
{
    using namespace util::literals;
    if (pns.ns_ < 1_K)
    {
        os << pns.ns_ << " ns";
    }
    else if (pns.ns_ < 1_M)
    {
        os << 1.0 * pns.ns_ / 1_K << " us";
    }
    else if (pns.ns_ < 1_G)
    {
        os << 1.0 * pns.ns_ / 1_M << " ms";
    }
    else
    {
        os << 1.0 * pns.ns_ / 1_G << " s";
    }
    return os;
}

class pre_op
{
public:
    pre_op(uint64_t op) : op_(op)
    {
    }
    uint64_t op_;
};
using pre_num = pre_op;

inline std::ostream &operator<<(std::ostream &os, pre_op p)
{
    uint64_t op = p.op_;
    if (op < 1_K)
    {
        os << op;
    }
    else if (op < 1_M)
    {
        os << op / 1_K << " K";
    }
    else if (op < 1_G)
    {
        os << op / 1_M << " M";
    }
    else
    {
        os << op / 1_G << " G";
    }
    return os;
}

class pre_ops
{
public:
    pre_ops(uint64_t op, uint64_t ns, bool verbose = false)
        : op_(op), ns_(ns), verbose_(verbose)
    {
        ops_ = 1e9 * op_ / ns_;
    }
    pre_ops(double ops) : ops_(ops)
    {
        // unknown
        op_ = 0;
        ns_ = 0;
        verbose_ = false;
    }
    uint64_t op_;
    uint64_t ns_;
    double ops_;
    bool verbose_;
};

inline std::ostream &operator<<(std::ostream &os, pre_ops p)
{
    double ops = p.ops_;
    if (ops < 1_K)
    {
        os << ops << " ops";
    }
    else if (ops < 1_M)
    {
        os << ops / 1_K << " Kops";
    }
    else if (ops < 1_G)
    {
        os << ops / 1_M << " Mops";
    }
    else
    {
        os << ops / 1_G << " Gops";
    }
    if (p.verbose_)
    {
        os << "[" << p.op_ << " in " << pre_ns(p.ns_) << "]";
    }
    return os;
}

class pre_byte
{
public:
    pre_byte(uint64_t byte) : byte_(byte)
    {
    }
    uint64_t byte_;
};
inline std::ostream &operator<<(std::ostream &os, pre_byte b)
{
    if (b.byte_ < 1_KB)
    {
        os << b.byte_ << " B";
    }
    else if (b.byte_ < 1_MB)
    {
        os << 1.0 * b.byte_ / 1_KB << " KB";
    }
    else if (b.byte_ < 1_GB)
    {
        os << 1.0 * b.byte_ / 1_MB << " MB";
    }
    else if (b.byte_ < 1_TB)
    {
        os << 1.0 * b.byte_ / 1_GB << " GB";
    }
    else
    {
        os << 1.0 * b.byte_ / 1_TB << " TB";
    }
    return os;
}

class pre_bit
{
public:
    pre_bit(uint64_t byte) : byte_(byte)
    {
    }
    uint64_t byte_;
};
inline std::ostream &operator<<(std::ostream &os, pre_bit b)
{
    auto bit = b.byte_ * 8;
    if (bit < 1_Ki)
    {
        os << bit << " b";
    }
    else if (bit < 1_Mi)
    {
        os << 1.0 * bit / 1_Ki << " Kb";
    }
    else if (bit < 1_Gi)
    {
        os << 1.0 * bit / 1_Mi << " Mb";
    }
    else if (bit < 1_Ti)
    {
        os << 1.0 * bit / 1_Gi << " Gb";
    }
    else
    {
        os << 1.0 * bit / 1_Ti << " Tb";
    }
    return os;
}

class pre_bit_bw
{
public:
    pre_bit_bw(uint64_t byte_bw) : bw_(byte_bw)
    {
    }
    uint64_t bw_;
};
inline std::ostream &operator<<(std::ostream &os, pre_bit_bw bw)
{
    pre_bit b(bw.bw_);
    os << b;
    os << "ps";
    return os;
}

class pre_bw
{
public:
    pre_bw(uint64_t bw) : bw_(bw)
    {
    }
    uint64_t bw_;
};
inline std::ostream &operator<<(std::ostream &os, pre_bw b)
{
    pre_byte byte(b.bw_);
    os << byte;
    os << "ps";
    return os;
}

struct pre_qp
{
    pre_qp(ibv_qp *qp) : qp_(*qp)
    {
    }
    pre_qp(const ibv_qp &qp) : qp_(qp)
    {
    }
    const ibv_qp &qp_;
};

inline std::ostream &operator<<(std::ostream &os, pre_qp qp)
{
    os << "{QP: qpn: " << qp.qp_.qp_num << ", type: " << qp.qp_.qp_type << "}";
    return os;
}

struct pre_pcnt
{
    pre_pcnt(double p) : pcnt(p)
    {
    }
    double pcnt;
};

inline std::ostream &operator<<(std::ostream &os, pre_pcnt p)
{
    os << std::fixed;
    os << std::setprecision(2);
    os << 100.0 * p.pcnt << "%";

    // revert the formating modification
    os.copyfmt(std::ios(NULL));
    return os;
}

template <typename T>
struct pre_distribution
{
    pre_distribution(const OnePassBucketMonitor<T> &m) : m_(m)
    {
    }
    const OnePassBucketMonitor<T> &m_;
};

// template <typename T>
// size_t __within_nr(const std::vector<T> &vec, T val, double within)
// {
//     return std::count_if(vec.begin(), vec.end(), [val, within](const T &t) {
//         return t >= (1.0 * val * (1 - within)) &&
//                t <= (1.0 * val * (1 + within));
//     });
// }

template <typename T>
inline std::ostream &operator<<(std::ostream &os, const pre_distribution<T> &p)
{
    auto m = p.m_;  // copy
    m.as_distribution();
    const auto &vec = m.buckets();

    auto max_val = m.max_as_distribution();
    auto min_val = m.min_as_distribution();

    auto collected = m.data_nr();
    auto [_, not_zero_pcnt] = m.not_zero_as_distribution();

    double hit_max_bucket_percent = 1.0 * max_val / collected;

    os << "{Dist: " << PRE(max_val) << ", " << PRE(min_val) << ", "
       << PRE(collected) << ", " << PRE(pre_pcnt(hit_max_bucket_percent))
       << ", " << PRE(pre_pcnt(not_zero_pcnt));

    os << std::endl;
    for (double pcnt : {0.0001, 0.001, 0.01, 0.1, 0.2, 0.5, 0.9})
    {
        auto [val_idx, val] = m.percentile_as_distribution(pcnt);
        auto val_is_pcnt_of_max = 1.0 * val / max_val;
        os << pre_pcnt(pcnt) << " bucket >= " << val << ", which has "
           << val_idx
           << " buckets and whose hit >= " << pre_pcnt(val_is_pcnt_of_max)
           << " of max bucket" << std::endl;
    }
    os << ", details: " << util::pre(vec, 10) << "}";
    return os;
}

}  // namespace util

// inline std::ostream &operator<<(std::ostream &os,
//                                 [[maybe_unused]] const ibv_send_wr &v)
// {
//     os << "{ibv_send_wr ";
//     os << "wr_id: " << util::pre(v.wr_id);
//     os << ", next: " << util::pre(v.next);
//     os << ", num_sge: " << util::pre(v.num_sge);
//     os << ", opcode: " << util::pre(v.opcode);
//     os << ", send_flags: " << util::pre(v.send_flags);
//     os << ", imm_data: " << util::pre(v.imm_data);
//     os << "}";
//     return os;
// }