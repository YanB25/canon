#include "Directory.h"

#include <glog/logging.h>
#include <gperftools/profiler.h>

#include "Connection.h"
#include "GlobalAddress.h"
#include "memory/block_allocator.h"
#include "util/Numa.h"
#include "util/Util.h"
#include "util/stacktrace.h"

namespace sherman
{
GlobalAddress g_root_ptr = GlobalAddress::Null();
int g_root_level = -1;
bool enable_cache{false};
}  // namespace sherman

std::shared_ptr<Directory> Directory::newInstance(
    DirectoryConnection &dCon,
    const std::vector<RemoteConnection> &remoteInfo,
    uint32_t machineNR,
    uint64_t dirID,
    uint16_t nodeID,
    size_t bind_core,
    void *dsm_base_addr,
    ::mem::BaseAllocator::pointer alloc)
{
    return std::make_shared<Directory>(dCon,
                                       remoteInfo,
                                       machineNR,
                                       dirID,
                                       nodeID,
                                       bind_core,
                                       dsm_base_addr,
                                       alloc);
}

Directory::Directory(DirectoryConnection &dCon,
                     const std::vector<RemoteConnection> &remoteInfo,
                     uint32_t machineNR,
                     uint16_t dirID,
                     uint16_t nodeID,
                     size_t bind_core,
                     void *dsm_base_addr,
                     ::mem::BaseAllocator::pointer alloc)
    : dCon(dCon),
      remoteInfo(remoteInfo),
      machineNR(machineNR),
      dirID(dirID),
      nodeID(nodeID),
      bind_core_(bind_core),
      dsm_base_addr_(dsm_base_addr),
      alloc_(alloc)
{
    dirTh = std::thread(&Directory::dirThread, this);
}

Directory::~Directory()
{
    dirTh.join();
}

void Directory::dirThread()
{
    auto &numa_ctl = util::NUMACtl::tl_ins();
    auto cpu_nr = numa_ctl.cpu_nr();
    // don't call util::get_thread_id() here
    // because dont want to fetch one thread it for him.
    // NOTE: bind to the last cpu core.
    auto assume_thread_id = cpu_nr - 1;
    CHECK(numa_ctl.try_set_core_affinity_by_thread_id(assume_thread_id));
    LOG(INFO) << "dir " << dirID << " launch at " << numa_ctl;

    while (!exit_.load(std::memory_order_acq_rel))
    {
        struct ibv_wc wc;
        auto cnt = pollOnce(dCon.rpc_cq, 1, &wc);
        DCHECK_GE(cnt, 0);
        DCHECK_LE(cnt, 1);
        if (cnt)
        {
            switch (int(wc.opcode))
            {
            case IBV_WC_RECV:  // control message
            {
                auto *m = (RawMessage *) dCon.message->getMessage();

                process_message(m);

                break;
            }
            case IBV_WC_RDMA_WRITE:
            {
                break;
            }
            case IBV_WC_RECV_RDMA_WITH_IMM:
            {
                break;
            }
            default:
                LOG(FATAL) << "** Unknown message.";
            }
        }
    }

    LOG(INFO) << "dirThread exits...";
}

void Directory::process_message(const RawMessage *m)
{
    RawMessage *send = nullptr;
    switch (m->type)
    {
    case RpcType::MALLOC:
    {
        send = (RawMessage *) dCon.message->getSendPool();

        // send->addr = chunckAlloc->alloc(m->alloc_size);

        auto alignment = 1ull << m->alloc_la;
        alignment = std::max(alignment, 8ull);  // at least 8B
        LOG(INFO) << "[dir] allocating " << m->alloc_size << " ("
                  << util::pre_byte(m->alloc_size) << ")";
        auto *get = alloc_->alloc(m->alloc_size, alignment);
        DCHECK_EQ((uint64_t) get % alignment, 0);
        if (unlikely(get == nullptr))
        {
            LOG_FIRST_N(WARNING, 1) << "[Directory] run out of memory.";
            auto ret = GlobalAddress::Null();
            ret.nodeID = nodeID;
            send->addr = ret;
        }
        else
        {
            DCHECK_GE(get, dsm_base_addr_) << "underflow detected.";
            auto offset = (char *) get - (char *) dsm_base_addr_;
            DCHECK_GT(offset, 0) << "offset == 0 is reserved for nullptr";
            GlobalAddress gaddr;
            gaddr.nodeID = nodeID;
            gaddr.offset = offset;
            send->addr = gaddr;
        }

        break;
    }

    case RpcType::NEW_ROOT:
    {
        if (sherman::g_root_level < m->level)
        {
            sherman::g_root_ptr = m->addr;
            sherman::g_root_level = m->level;
            if (sherman::g_root_level >= 4)
            {
                LOG(ERROR) << "ENABLE_CACHE";
                sherman::enable_cache = true;
            }
        }

        break;
    }

    default:
        assert(false);
    }

    if (send)
    {
        dCon.sendMessage2App(send, m->node_id, m->app_id);
    }
}

// void Directory::sendData2App(const RawMessage *m) {
//   rdmaWrite(dCon->QPs[m->appID][m->nodeID], (uint64_t)dCon->dsmPool,
//             m->destAddr, 1024, dCon->dsmLKey,
//             remoteInfo[m->nodeID].appRKey[m->appID], 11, true, 0);
// }
