#pragma once
#ifndef __THREADCONNECTION_H__
#define __THREADCONNECTION_H__

#include <vector>

#include "Common.h"
#include "RawMessageConnection.h"
#include "mlx5/mlx5.h"
#include "rdmacpp/QP.h"
// #include "rdmacpp/CQP.h"
// #include "rdmacpp/CQP2.h"

struct RemoteConnection;

/**
 * @brief ThreadConnection is an abstraction of application thread
 */
struct ThreadConnection
{
    ThreadConnection(uint16_t threadID,
                     const std::string &rnic,
                     void *cachePool,
                     uint64_t cacheSize,
                     uint32_t machineNR,
                     const std::vector<RemoteConnection> &remoteInfo);
    ThreadConnection(const ThreadConnection &) = delete;
    ThreadConnection &operator=(const ThreadConnection &) = delete;
    ThreadConnection &operator=(ThreadConnection &&rhs)
    {
        ctx = std::move(rhs.ctx);
        threadID = std::move(rhs.threadID);
        cq_ = std::move(rhs.cq_);
        rhs.cq_ = nullptr;
        rpc_cq = std::move(rhs.rpc_cq);
        rhs.rpc_cq = nullptr;
        message = std::move(rhs.message);
        rhs.message = nullptr;
        QPs = std::move(rhs.QPs);
        cacheMR = std::move(rhs.cacheMR);
        rhs.cacheMR = nullptr;
        cachePool = std::move(rhs.cachePool);
        rhs.cachePool = nullptr;
        cacheLKey = std::move(rhs.cacheLKey);
        remoteInfo = std::move(rhs.remoteInfo);
        rhs.remoteInfo = nullptr;
        return *this;
    }
    ibv_cq *ibcq()
    {
        return cq_->ibcq();
    }
    rdma::CQ *cq()
    {
        return cq_.get();
    }
    ThreadConnection(ThreadConnection &&rhs)
    {
        (*this) = std::move(rhs);
    }
    bool resetQP(size_t node_id, size_t dir_id);
    void sendMessage2Dir(RawMessage *m, uint16_t node_id, uint16_t dir_id = 0);
    ~ThreadConnection();
    constexpr ibv_pd *pd()
    {
        return ctx.pd;
    }

    RdmaContext ctx;
    uint16_t threadID{0};

    // /**
    //  * cq is a shared completion queue used in Reliable Connection QPs
    //  */
    // ibv_cq *cq{nullptr};
    rdma::CQ::Pointer cq_;
    /**
     * cq is a completion queue used in Unreliable Datagram QP
     */
    ibv_cq *rpc_cq{nullptr};

    RawMessageConnection *message{nullptr};

    /**
     * @brief maintain QPs[NR_DIRECTORY][machineNR]
     */
    std::vector<std::vector<rdma::QP>> QPs;

    std::vector<std::vector<mlx5::IQP>> iQPs;

    ibv_exp_res_domain *res_dom_{nullptr};

    // concurrent QPs
    // std::array<std::array<rdma::CQP, MAX_MACHINE>, NR_DIRECTORY> cQPs{};

    ibv_mr *cacheMR{nullptr};
    void *cachePool{nullptr};
    uint32_t cacheLKey{0};
    const std::vector<RemoteConnection> *remoteInfo{nullptr};
};

#endif /* __THREADCONNECTION_H__ */
