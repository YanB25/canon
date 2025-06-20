#pragma once
#ifndef __DIRECTORYCONNECTION_H__
#define __DIRECTORYCONNECTION_H__

#include <vector>

#include "Common.h"
#include "DSMConfig.h"
#include "HugePageAlloc.h"
#include "RawMessageConnection.h"
#include "rdmacpp/MR.h"
#include "rdmacpp/QP.h"

struct RemoteConnection;

// directory thread
struct DirectoryConnection
{
    DirectoryConnection(uint16_t dirID,
                        const std::string &rnic,
                        void *dsmPool,
                        uint64_t dsmSize,
                        uint32_t machineNR,
                        std::vector<RemoteConnection> &remoteInfo,
                        const DSMConfig &conf);
    DirectoryConnection(DirectoryConnection &) = delete;
    DirectoryConnection &operator=(DirectoryConnection &) = delete;
    DirectoryConnection &operator=(DirectoryConnection &&rhs)
    {
        ctx = std::move(rhs.ctx);
        dirID = rhs.dirID;
        cq_ = std::move(rhs.cq_);
        rhs.cq_ = nullptr;
        rpc_cq = rhs.rpc_cq;
        rhs.rpc_cq = nullptr;
        message = rhs.message;
        rhs.message = nullptr;
        QPs = std::move(rhs.QPs);
        dsmMR = rhs.dsmMR;
        rhs.dsmMR = nullptr;
        dsmPool = rhs.dsmPool;
        rhs.dsmPool = nullptr;
        dsmSize = rhs.dsmSize;
        dsmLKey = rhs.dsmLKey;
        lockMR = rhs.lockMR;
        rhs.lockMR = nullptr;
        dmPool = rhs.dmPool;
        rhs.dmPool = nullptr;
        lockSize = rhs.lockSize;
        lockLKey = rhs.lockLKey;
        remoteInfo = std::move(rhs.remoteInfo);
        rhs.remoteInfo = nullptr;
        return *this;
    }
    DirectoryConnection(DirectoryConnection &&rhs)
    {
        *this = std::move(rhs);
    }

    rdma::MemoryRegion create_secondary_mr(size_t size)
    {
        auto *buffer = CHECK_NOTNULL(hugePageAlloc(size));
        auto *mr = createMemoryRegion((uint64_t) buffer, size, &ctx);
        return rdma::MemoryRegion{
            .addr = buffer,
            .length = size,
            .lkey = mr->lkey,
            .rkey = mr->rkey,
            .mr = CHECK_NOTNULL(mr),
        };
    }
    ibv_cq *ibcq()
    {
        return cq_->ibcq();
    }
    rdma::CQ *cq()
    {
        return cq_.get();
    }
    void free_secondary_mr(rdma::MemoryRegion mr)
    {
        CHECK(destroyMemoryRegion(mr.mr));
    }

    void sendMessage2App(RawMessage *m, uint16_t node_id, uint16_t th_id);
    ~DirectoryConnection();

    RdmaContext ctx;  // does not own
    uint16_t dirID{0};

    /**
     * cq is a shared completion queue used in Reliable Connection QPs
     */
    rdma::CQ::Pointer cq_;
    /**
     * cq is a completion queue used in Unreliable Datagram QP
     */
    ibv_cq *rpc_cq{nullptr};

    std::shared_ptr<RawMessageConnection> message;

    /**
     * @brief maintain QPs[kMaxAppThread][machineNR]
     */
    std::vector<std::vector<rdma::QP>> QPs;

    ibv_mr *dsmMR{nullptr};
    void *dsmPool{nullptr};
    uint64_t dsmSize{0};
    uint32_t dsmLKey{0};

    ibv_mr *lockMR{nullptr};
    void *dmPool{nullptr};  // address on-chip
    uint64_t lockSize{0};
    uint32_t lockLKey{0};

    std::vector<RemoteConnection> *remoteInfo;

    size_t machine_nr_{0};
};

#endif /* __DIRECTORYCONNECTION_H__ */
