#include "DirectoryConnection.h"

#include <glog/logging.h>
#include <infiniband/verbs_exp.h>

#include <chrono>
#include <thread>

#include "Connection.h"
#include "Timer.h"
#include "util/Pre.h"
using namespace std::chrono_literals;

/**
 * every DirectoryConnection has its own protection domain
 */
DirectoryConnection::DirectoryConnection(
    uint16_t dirID,
    const std::string &rnic,
    void *dsmPool,
    uint64_t dsmSize,
    uint32_t machineNR,
    std::vector<RemoteConnection> &remoteInfo,
    const DSMConfig &conf)
    : dirID(dirID), remoteInfo(&remoteInfo), machine_nr_(machineNR)
{
    DefOnceContTimer(timer,
                     config::kMonitorControlPath,
                     "DirectoryConnection::DirectoryConnection()");

    CHECK(createContext(&ctx, rnic));
    timer.pin("createContext");

    cq_ = rdma::CQ::make_ptr(ctx.ctx, RAW_RECV_CQ_COUNT, nullptr, nullptr, 0);
    rpc_cq =
        CHECK_NOTNULL(ibv_create_cq(ctx.ctx, RAW_RECV_CQ_COUNT, NULL, NULL, 0));

    timer.pin("2x ibv_create_cq");
    message = RawMessageConnection::newInstance(ctx, rpc_cq, DIR_MESSAGE_NR);

    message->initRecv();
    message->initSend();
    timer.pin("message");

    // dsm memory
    this->dsmPool = dsmPool;
    this->dsmSize = dsmSize;
    uint64_t flags = 0;
    if (conf.relaxed_ordering)
    {
        flags |= IBV_EXP_ACCESS_RELAXED_ORDERING;
    }
    dsmMR = CHECK_NOTNULL(
        createMemoryRegion((uint64_t) dsmPool, dsmSize, &ctx, flags));
    this->dsmLKey = dsmMR->lkey;
    timer.pin("createMR");
    DVLOG(config::verbose::kDump) << "[MR] " << PRE((void *) dsmPool) << ", "
                                  << PRE(dsmSize) << ", " << PRE(dsmMR->rkey);

    if (dirID == 0)
    {
        this->dmPool = (void *) define::kLockStartAddr;
        this->lockSize = define::kLockChipMemSize;
        this->lockMR = CHECK_NOTNULL(
            createMemoryRegionOnChip((uint64_t) dmPool, lockSize, &ctx));
        this->lockLKey = lockMR->lkey;
    }

    // app, RC
    for (int i = 0; i < kMaxAppThread; ++i)
    {
        QPs.emplace_back();
        for (size_t k = 0; k < machineNR; ++k)
        {
            uint32_t create_flags = 0;
            create_flags |= IBV_EXP_QP_CREATE_UMR;
            auto max_wr = 128;
            auto max_sge = 1;
            rdma::QP qp(ctx.pd,
                        ctx.ctx,
                        IBV_QPT_RC,
                        cq_,
                        max_wr,
                        max_wr,
                        max_sge,
                        max_sge,
                        0,
                        nullptr,
                        create_flags);
            QPs.back().emplace_back(std::move(qp));
        }
    }
    timer.pin("create QPs");

    timer.pin("reliable recv");
    timer.report();
}

void DirectoryConnection::sendMessage2App(RawMessage *m,
                                          uint16_t node_id,
                                          uint16_t th_id)
{
    message->sendRawMessage(m,
                            (*remoteInfo)[node_id].appMessageQPN[th_id],
                            (*remoteInfo)[node_id].dirToAppAh[dirID][th_id]);
}
DirectoryConnection::~DirectoryConnection()
{
    DefOnceContTimer(
        timer, config::kMonitorControlPath, "~DirectoryConnection()");

    QPs.clear();

    if (dirID == 0)
    {
        CHECK(destroyMemoryRegionOnChip(lockMR, ctx.dm));
        lockMR = nullptr;
    }
    timer.pin("destroy off-chip MRs");

    CHECK(destroyMemoryRegion(dsmMR));
    dsmMR = nullptr;

    if (message)
    {
        message->destroy();
    }
    timer.pin("destroy message");

    cq_.reset();

    CHECK(destroyCompleteQueue(rpc_cq));
    rpc_cq = nullptr;
    timer.pin("destroy cqs");
    // must free AH before freeing PD, otherwise it crashes when trying to free
    // AH.
    for (size_t node_id = 0; node_id < machine_nr_; ++node_id)
    {
        for (size_t i = 0; i < kMaxAppThread; ++i)
        {
            ibv_ah *pah = (*remoteInfo)[node_id].dirToAppAh[dirID][i];
            if (unlikely(pah == nullptr))
            {
                continue;
            }

            PLOG_IF(ERROR, ibv_destroy_ah(pah)) << "failed to destroy ah";
            (*remoteInfo)[node_id].dirToAppAh[dirID][i] = nullptr;
        }
    }

    CHECK(destroyContext(&ctx));
    timer.pin("destroy context (dealloc PD, close device)");
    timer.report();
}