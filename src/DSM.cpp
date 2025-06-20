#include "DSM.h"

#include <algorithm>
#include <limits>
#include <memory>

#include "Common.h"
#include "DSMKeeper.h"
#include "Directory.h"
#include "HugePageAlloc.h"
#include "Pool.h"
#include "RPC.h"
#include "Rdma.h"
#include "Timer.h"
#include "jemalloc_cpp/jemalloc_cpp.h"
#include "memory/dsm_allocator.h"
#include "rdmacpp/WRCtx.h"
#include "umsg/UnreliableConnection.h"
#include "util/Tracer.h"
#include "util/Util.h"

using namespace rpc;

thread_local int DSM::thread_id_ = -1;
thread_local int DSM::thread_name_id_ = -1;
thread_local ThreadConnection *DSM::iCon_ = nullptr;
// [machine][directory]
// to enable dsm_->alloc(size) and dsm_->free(gaddr, size)
thread_local mem::LazySlabAllocator::pointer DSM::dsm_local_allocator_;
thread_local RdmaBuffer DSM::rbuf_[define::kMaxCoroNr];
thread_local uint64_t DSM::thread_tag_ = 0;
thread_local util::MetricCollector DSM::c_;
thread_local Jemalloc::JemallocAllocator<Jemalloc::Tag::RDMA_Buf>::pointer
    DSM::rdma_buf_allocator_;
thread_local std::vector<RdmaOperationBatch> DSM::rdma_op_batch_;
thread_local std::vector<CoroAllocCtx> DSM::coro_alloc_ctx_;

std::shared_ptr<DSM> DSM::getInstance(const DSMConfig &conf)
{
    return std::make_shared<DSM>(conf);
}

DSM::DSM(const DSMConfig &conf) : conf(conf), cache(conf.cacheConfig)
{
    baseAddrSize = buffer_size();
    baseAddr = (uint64_t) hugePageAlloc(baseAddrSize);
    while ((void *) baseAddr == nullptr)
    {
        LOG(ERROR) << "[dsm] Failed to hugePageAlloc for size " << baseAddrSize
                   << ". Sleep for a while and retry";
        std::this_thread::sleep_for(5s);
        baseAddr = (uint64_t) hugePageAlloc(baseAddrSize);
    }
    LOG(INFO) << "[DSM] Total buffer: "
              << Buffer((char *) baseAddr, baseAddrSize);

    // warmup
    // for (uint64_t i = baseAddr; i < baseAddr + baseAddrSize; i += 2_MB)
    // {
    //     *(char *) i = 0;
    // }

    // clear up first chunk
    // memset((char *) baseAddr, 0, define::kChunkSize);

    initRDMAConnection();

    keeper->barrier("DSM-init", 1ms);

    auto nid = get_node_id();

    using Jemalloc::Tag;
    // this allocator manages the whole DSM address space
    // in the unit of blocks
    // NOTE: advance 4KB so that we never return (offset == 0),
    // which interprets into nullptr
    g_dsm_allocator_ = std::make_shared<::mem::RollingAllocator>(
        (char *) baseAddr + 4_KB, baseAddrSize - 4_KB);
    Jemalloc::prepare_allocator<Tag::DSM>(g_dsm_allocator_.get());

    // RDMA buffers
    g_rdma_buf_allocator_ = std::make_shared<mem::RollingAllocator>(
        (void *) cache.data, cache.size);
    Jemalloc::prepare_allocator<Tag::RDMA_Buf>(g_rdma_buf_allocator_.get());

    ssize_t cur_bind_core = kCorePerNuma;
    for (size_t dir_id = 0; dir_id < conf.dir_thread_nr; ++dir_id)
    {
        CHECK_GE(cur_bind_core, 0);
        LOG(INFO) << "DIR: " << PRE(cur_bind_core);

        auto allocator =
            Jemalloc::JemallocAllocator<Tag::DSM>::make_allocator();
        dir_agent_.emplace_back(Directory::newInstance(*dirCon[dir_id],
                                                       remoteInfo,
                                                       conf.machineNR,
                                                       dir_id,
                                                       nid,
                                                       cur_bind_core,
                                                       (void *) baseAddr,
                                                       allocator));
        cur_bind_core--;
    }

    if (conf.worker_nr)
    {
        for (size_t wid = 0; wid < conf.worker_nr; ++wid)
        {
            auto allocator =
                Jemalloc::JemallocAllocator<Tag::DSM>::make_allocator();
            workers_.emplace_back(std::make_unique<Worker>(
                this, wid, (void *) baseAddr, allocator));

            workers_.back()->launch();
        }
    }

    explain();
}

void DSM::initExchangeMetadataBootstrap()
{
    CHECK(hasRegistered());

    for (size_t node_id = 0; node_id < getClusterSize(); ++node_id)
    {
        const auto &src_meta = keeper->getExchangeMeta(node_id);
        auto &dst_meta = getExchangeMetaBootstrap(node_id);
        memcpy(&dst_meta, &src_meta, sizeof(ExchangeMeta));
    }
}

void DSM::syncMetadataBootstrap(const ExchangeMeta &self_meta, size_t remoteID)
{
    const auto &src_meta = self_meta;

    if (remoteID == get_node_id())
    {
        auto &dst_meta = getExchangeMetaBootstrap(remoteID);
        memcpy(&dst_meta, &src_meta, sizeof(dst_meta));
    }
    else
    {
        GlobalAddress gaddr;
        gaddr.nodeID = remoteID;
        gaddr.offset = get_node_id() * sizeof(ExchangeMeta);
        auto rdma_buffer = get_rdma_buffer(sizeof(ExchangeMeta));
        auto *buffer = rdma_buffer.buffer;
        DCHECK_LT(sizeof(ExchangeMeta), rdma_buffer.size);
        memcpy(buffer, &src_meta, sizeof(src_meta));
        write_sync(buffer, gaddr, sizeof(ExchangeMeta), nullptr);
    }
}

DSM::~DSM()
{
    keeper_barrier("DSM::~DSM()", 100ms);
    // dtor dir agents first.
    for (auto &dir : dir_agent_)
    {
        dir->signal_exit();
    }
    dir_agent_.clear();  // joining dir_agent threads auto-ly

    for (auto &w : workers_)
    {
        w->signal_exit();
        w->join();
    }
    workers_.clear();  // joining worker threads auto-ly

    // the reverse order of DSM::initRDMAConnection()
    for (size_t node_id = 0; node_id < remoteInfo.size(); ++node_id)
    {
        remoteInfo[node_id].destroy();
    }
    thCon.clear();
    dirCon.clear();

    CHECK(hugePageFree((void *) baseAddr, baseAddrSize));
}

ExchangeMeta &DSM::getExchangeMetaBootstrap(size_t node_id) const
{
    size_t my_node_id = get_node_id();
    char *start_addr = (char *) remoteInfo[my_node_id].dsmBase;
    char *meta_start_addr = start_addr + node_id * sizeof(ExchangeMeta);
    return *(ExchangeMeta *) meta_start_addr;
}

bool DSM::reinitializeDir(size_t dirID)
{
    ContTimer<config::kMonitorReconnection> timer("DSM::reinitialzeDir");
    LOG(INFO) << "[DSM] Reinitialize DirectoryConnetion[" << dirID << "]";

    CHECK_LT(dirID, dirCon.size());
    // here destroy connection
    dirCon[dirID].reset();
    dirCon[dirID] = std::make_unique<DirectoryConnection>(dirID,
                                                          conf.rnic,
                                                          (void *) baseAddr,
                                                          baseAddrSize,
                                                          conf.machineNR,
                                                          remoteInfo,
                                                          conf);

    timer.pin("Reinit DirConnection");

    // update the boostrapped exchangeMeta for all the peers
    for (size_t remoteID = 0; remoteID < getClusterSize(); ++remoteID)
    {
        auto ex = keeper->updateDirMetadata(*dirCon[dirID], remoteID);
        // DVLOG(1) << "[DSM] update dir meta for " << remoteID
        //          << ", hash: "
        //          << util::pre_hex(util::djb2_digest((char *) &ex,
        //          sizeof(ex)))
        //          << ", rkey: " << ex.dirTh[dirID].rKey;
        syncMetadataBootstrap(ex, remoteID);
        auto connect_dir_ex = getExchangeMetaBootstrap(remoteID);

        for (size_t app_id = 0; app_id < kMaxAppThread; ++app_id)
        {
            keeper->connectDir(
                *dirCon[dirID], remoteID, app_id, connect_dir_ex);
        }
    }
    timer.pin("Reconnect to ThreadConnections");
    timer.report();
    return true;
}

bool DSM::reconnectThreadToDir(size_t node_id, size_t dirID)
{
    ContTimer<config::kMonitorReconnection> timer("DSM::reconnectThreadToDir");
    DCHECK_LT(dirID, NR_DIRECTORY);

    LOG(INFO) << "[DSM] reconnect ThreadConnection for node " << node_id
              << ", dir " << dirID;

    for (size_t i = 0; i < kMaxAppThread; ++i)
    {
        if (!thCon[i]->resetQP(node_id, dirID))
        {
            LOG(WARNING)
                << "[DSM] failed to resetQP for ThreadConnection. thCon[" << i
                << "]";
            return false;
        }
        const auto &cur_meta = getExchangeMetaBootstrap(node_id);

        DVLOG(::config::verbose::kSystem)
            << "[DSM] reconnecting ThreadConnection[" << i
            << "]. node_id: " << node_id << ", dirID: " << dirID
            << ", meta digest: "
            << util::pre_hex(util::djb2_digest((const char *) &cur_meta,
                                               sizeof(cur_meta)))
            << ", rkey: " << cur_meta.dirTh[dirID].rKey;

        keeper->connectThread(*thCon[i], node_id, dirID, cur_meta);

        keeper->updateRemoteConnectionForDir(
            remoteInfo[node_id], cur_meta, dirID);
    }
    timer.pin("end");
    timer.report();
    return true;
}

bool DSM::recoverThreadQP(int node_id, size_t dirID, util::TraceView v)
{
    auto tid = get_thread_id();
    ibv_qp *qp = get_th_qp(node_id, dirID);
    DVLOG(::config::verbose::kSystem)
        << "Recovering th qp: " << qp << ". node_id: " << node_id
        << ", thread_id: " << tid;
    const auto &ex = getExchangeMetaBootstrap(node_id);

    if (!modifyErrQPtoNormal(qp,
                             ex.dirRcQpn2app[dirID][tid],
                             ex.dirTh[dirID].lid,
                             ex.dirTh[dirID].gid,
                             &iCon_->ctx))
    {
        LOG(ERROR) << "failed to modify th QP to normal state. node_id: "
                   << node_id << ", thread_id: " << tid;
        return false;
    }
    rdmaQueryQueuePair(qp);

    v.pin("client-recovery");
    return true;
}

bool DSM::recoverDirQP(int node_id, int thread_id, size_t dirID)
{
    ibv_qp *qp = get_dir_qp(node_id, thread_id, dirID);
    DVLOG(::config::verbose::kSystem)
        << "Recovering dir qp " << qp << ". node_id: " << node_id
        << ", thread_id: " << thread_id;
    const auto &ex = getExchangeMetaBootstrap(node_id);
    if (!modifyErrQPtoNormal(qp,
                             ex.appRcQpn2dir[thread_id][dirID],
                             ex.appTh[thread_id].lid,
                             ex.appTh[thread_id].gid,
                             &dirCon[dirID]->ctx))
    {
        LOG(ERROR) << "failed to modify dir QP to normal state. node: "
                   << node_id << ", tid: " << thread_id;
        return false;
    }

    return true;
}

ThreadResourceDesc DSM::getCurrentThreadDesc()
{
    ThreadResourceDesc desc;
    desc.thread_id = thread_id_;
    desc.thread_tag = thread_tag_;
    desc.icon = iCon_;
    return desc;
}

ThreadResourceDesc DSM::prepareThread()
{
    ThreadResourceDesc desc;

    desc.thread_id = util::get_thread_id();
    CHECK_LT(desc.thread_id, (int) thCon.size())
        << "Can not allocate more threads";
    desc.thread_tag =
        desc.thread_id + (((uint64_t) this->getMyNodeID()) << 32) + 1;

    desc.icon = thCon[desc.thread_id].get();

    desc.icon->message->initRecv();
    desc.icon->message->initSend();

    // CHECK_LT(desc.thread_id * define::kRDMABufferSize, cache.size)
    //     << "Run out of cache size for offset = "
    //     << desc.thread_id * define::kRDMABufferSize;

    return desc;
}

bool DSM::hasRegistered() const
{
    return thread_id_ != -1;
}

bool DSM::applyResource(const ThreadResourceDesc &desc, bool bind_core)
{
    thread_id_ = desc.thread_id;
    if (unlikely(thread_name_id_ == -1))
    {
        thread_name_id_ = thread_id_;
    }
    thread_tag_ = desc.thread_tag;
    iCon_ = desc.icon;

    using Jemalloc::Tag;
    auto rdma_buf_allocator =
        Jemalloc::JemallocAllocator<Tag::RDMA_Buf>::get_thread_allocator();
    rdma_buf_allocator_ = rdma_buf_allocator;

    if (bind_core)
    {
        CHECK_EQ(thread_id_, util::get_thread_id());
        auto &numa_ctl = util::NUMACtl::tl_ins();
        CHECK(numa_ctl.try_set_core_affinity_by_thread_id(thread_id_));
    }
    VLOG(SV) << "[DSM] thread applying to: " << desc;

    if (unlikely(thread_id_ == 0))
    {
        // After the system boot, we do not rely on memcached anymore (and it is
        // slow.) we use RDMA itself to maintain metadata (i.e., in the
        // *bootstrap* way).
        LOG(INFO)
            << "[DSM] thread tid == 0 initing exchange metadata (bootstrap)...";
        initExchangeMetadataBootstrap();
    }

    for (size_t i = 0; i < define::kMaxCoroNr; ++i)
    {
        auto coro_rdma_buf = get_rdma_buffer(define::kPerCoroRdmaBuf);
        rbuf_[i].set_buffer(std::move(coro_rdma_buf));
    }

    return true;
}

bool DSM::registerThread()
{
    if (hasRegistered())
    {
        return false;
    }

    rdma_op_batch_.resize(define::kMaxCoroNr);

    auto desc = prepareThread();
    auto succ = applyResource(desc, true);
    LOG_IF(WARNING, !succ) << "[DSM] failed to apply resource.";

    // this is client-side: RPC-alloc DSM with local cache
    dsm_allocator_.current() = std::make_shared<memory::DSMAllocator>(this);

    // this is server-side: allocate the *local* DSM
    dsm_local_allocator_ =
        Jemalloc::JemallocAllocator<Jemalloc::Tag::DSM>::get_thread_allocator();

    for (size_t c = 0; c < define::kMaxCoroNr; ++c)
    {
        auto expect_nid = (get_node_id() + 1 + c) % getClusterSize();
        coro_alloc_ctx_.emplace_back(CoroAllocCtx{
            .buf = nullptr,
            .buf_size = 0,
            .cur_nid = expect_nid,
        });
    }

    return true;
}

void DSM::initRDMAConnection()
{
    ContTimer<config::kMonitorControlPath> timer("DSM::initRDMAConnection()");

    LOG(INFO) << "Machine NR: " << conf.machineNR;

    remoteInfo.resize(conf.machineNR);

    for (int i = 0; i < kMaxAppThread; ++i)
    {
        thCon.emplace_back(
            std::make_unique<ThreadConnection>(i,
                                               conf.rnic,
                                               (void *) cache.data,
                                               cache.size,
                                               conf.machineNR,
                                               remoteInfo));
    }
    timer.pin("thCons " + std::to_string(kMaxAppThread));

    for (int i = 0; i < NR_DIRECTORY; ++i)
    {
        dirCon.emplace_back(
            std::make_unique<DirectoryConnection>(i,
                                                  conf.rnic,
                                                  (void *) baseAddr,
                                                  baseAddrSize,
                                                  conf.machineNR,
                                                  remoteInfo,
                                                  conf));
    }
    timer.pin("dirCons " + std::to_string(NR_DIRECTORY));

    umsg_ = std::make_unique<UnreliableConnection<kCorePerNuma>>(
        cache.data, cache.size, conf.rnic, remoteInfo);

    timer.pin("keeper init");

    // thCon, dirCon, remoteInfo set up here.
    keeper = DSMKeeper::newInstance(
        thCon, dirCon, *umsg_, remoteInfo, conf.machineNR);
    timer.pin("keeper init");

    myNodeID = keeper->getMyNodeID();
    timer.report();

    auto DV = ::config::verbose::kDump;
    for (size_t i = 0; i < thCon.size(); ++i)
    {
        ibv_qp *qp = thCon[i]->message->get_message_qp();
        VLOG(DV) << "[dump] thCon[" << i << "] QP: " << util::pre_qp(qp);
    }
    for (size_t i = 0; i < dirCon.size(); ++i)
    {
        auto *qp = dirCon[i]->message->get_message_qp();
        VLOG(DV) << "[dump] dirCon[" << i << "] QP: " << util::pre_qp(qp);
    }

    for (size_t i = 0; i < getClusterSize(); ++i)
    {
        std::vector<uint32_t> arr(std::begin(remoteInfo[i].dirMessageQPN),
                                  std::end(remoteInfo[i].dirMessageQPN));
        VLOG(DV) << "[dump] remoteInfo[" << i
                 << "].dirMessageQPN: " << util::pre(arr);
    }
}

GlobalAddress DSM::alloc(size_t size, size_t alignment)
{
    DCHECK(hasRegistered());
    auto ret = dsm_allocator_.current()->alloc(size, alignment);
    DCHECK_EQ((uint64_t) ret.offset % alignment, 0);
    if (likely(!ret.is_null()))
    {
        dsm_usage_.current().record_alloc(size);
    }
    return ret;
}

GlobalAddress DSM::alloc_from(size_t size, size_t node_id, size_t alignment)
{
    DCHECK(hasRegistered());
    // TODO: use dir in round-robin way
    DCHECK_LE(node_id, std::numeric_limits<uint8_t>::max());
    auto ret = dsm_allocator_.current()->alloc_from(size, node_id, alignment);
    DCHECK_EQ((uint64_t) ret.offset % alignment, 0);
    DCHECK_EQ(ret.nodeID, node_id);
    if (likely(!ret.is_null()))
    {
        dsm_usage_.current().record_alloc(size);
    }
    return ret;
}
void DSM::free(GlobalAddress gaddr, size_t size)
{
    // TODO: use dir in round-robin way
    DCHECK_LE(gaddr.nodeID, std::numeric_limits<uint8_t>::max());
    if (likely(!gaddr.is_null()))
    {
        dsm_usage_.current().record_dealloc(size);
        dsm_allocator_.current()->free(gaddr, size);
    }
}

GlobalAddress DSM::rpc_alloc_from(size_t size, size_t to_nid, CoroContext *ctx)
{
    DCHECK(hasRegistered());
    auto coro_id = ctx ? ctx->coro_id() : 0;

    auto req_buf = get_rdma_buffer(sizeof(AllocRequest));
    AllocRequest *req = (AllocRequest *) req_buf.buffer;
    memset(req, 0, sizeof(AllocRequest));
    req->hdr.type = RPCType::kAlloc;
    req->hdr.rpc_context = 0 /* not used */;
    req->hdr.from_nid = get_node_id();
    req->hdr.from_epid = get_thread_id();
    req->hdr.from_coro_id = coro_id;
    req->size = size;
    req->flags = 0;
    auto to_dir = kServerStartEpId;

    uint64_t resp_gaddr = 0;

    unreliable_send(req_buf.buffer, sizeof(AllocRequest), to_nid, to_dir);

    if (likely(ctx != nullptr))
    {
        ctx->yield_to_master();
    }
    else
    {
        char resp_buf[128];
        unreliable_recv(resp_buf, 1);
        AllocResponse *resp = (AllocResponse *) resp_buf;
        resp_gaddr = resp->addr;
    }
    put_rdma_buffer(std::move(req_buf));

    GlobalAddress gaddr((void *) resp_gaddr);

    return gaddr;
}

GlobalAddress DSM::rpc_alloc(size_t size,
                             CoroContext *ctx,
                             const mem::Policy &p)
{
    DCHECK(hasRegistered());
    auto coro_id = ctx ? ctx->coro_id() : 0;
    auto &alloc_ctx = coro_alloc_ctx_[coro_id];

    auto *rpc_context = TLS<DPool<rpc::RpcContext>>().alloc();

    auto req_buf = get_rdma_buffer(sizeof(AllocRequest));
    AllocRequest *req = (AllocRequest *) req_buf.buffer;
    memset(req, 0, sizeof(AllocRequest));
    req->hdr.type = RPCType::kAlloc;
    req->hdr.rpc_context = (uint64_t) rpc_context;
    req->hdr.from_nid = get_node_id();
    req->hdr.from_epid = get_thread_id();
    req->hdr.from_coro_id = coro_id;
    req->size = size;
    req->flags = p.flags;
    auto to_dir = kServerStartEpId;

    uint64_t resp_gaddr = 0;
    rpc_context->data = &resp_gaddr;

    unreliable_send(
        req_buf.buffer, sizeof(AllocRequest), alloc_ctx.cur_nid, to_dir);
    alloc_ctx.cur_nid = (alloc_ctx.cur_nid + 1) % getClusterSize();

    if (likely(ctx != nullptr))
    {
        ctx->yield_to_master();
    }
    else
    {
        char resp_buf[128];
        unreliable_recv(resp_buf, 1);
        AllocResponse *resp = (AllocResponse *) resp_buf;
        resp_gaddr = resp->addr;
    }
    TLS<DPool<RpcContext>>().free(rpc_context);
    put_rdma_buffer(std::move(req_buf));

    GlobalAddress gaddr((void *) resp_gaddr);

    alloc_ctx.buf = (char *) resp_gaddr;
    alloc_ctx.buf_size = define::kChunkSize;
    CHECK_GE(alloc_ctx.buf_size, define::kChunkSize);

    auto *ret = alloc_ctx.buf;
    alloc_ctx.buf += size;
    alloc_ctx.buf_size -= size;
    return GlobalAddress(ret);
}

GlobalAddress DSM::alloc2(size_t size, CoroContext *ctx, const mem::Policy &p)
{
    auto coro_id = ctx ? ctx->coro_id() : 0;
    auto &alloc_ctx = coro_alloc_ctx_[coro_id];
    bool force_rpc = p.flags & (flag_t) mem::AllocFlag::kForceRemote;

    bool is_local = alloc_ctx.buf_size >= size && !force_rpc;
    if (likely(is_local))
    {
        void *ret = alloc_ctx.buf;
        alloc_ctx.buf_size -= size;
        alloc_ctx.buf += size;
        return GlobalAddress(ret);
    }
    else
    {
        auto alloc_size = std::max(size, p.batch_size_);

        auto got = rpc_alloc(alloc_size, ctx, p);
        alloc_ctx.buf_size = alloc_size;
        alloc_ctx.buf = (char *) got.val;

        auto *ret = alloc_ctx.buf;
        alloc_ctx.buf_size -= size;
        alloc_ctx.buf += size;
        return GlobalAddress(ret);
    }
}

void DSM::read(char *buffer,
               GlobalAddress gaddr,
               size_t size,
               bool signal,
               CoroContext *ctx)
{
    size_t dirID = get_cur_dir();

    uint32_t rkey = remoteInfo[gaddr.nodeID].dsmRKey[dirID];
    uint64_t wr_id = 0;
    if (ctx)
    {
        wr_id = ctx->coro_id();
        CHECK(signal);
    }
    c_.collect(gaddr, size, util::OpType::kRead);
    CHECK(rdmaRead(iCon_->QPs[dirID][gaddr.nodeID].ibqp(),
                   (uint64_t) buffer,
                   remoteInfo[gaddr.nodeID].dsmBase + gaddr.offset,
                   size,
                   iCon_->cacheLKey,
                   rkey,
                   signal,
                   wr_id));
    if (ctx)
    {
        ctx->yield_to_master();
    }
}
void DSM::read_sync(char *buffer,
                    GlobalAddress gaddr,
                    size_t size,
                    CoroContext *ctx)
{
    read(buffer, gaddr, size, true, ctx);

    if (ctx == nullptr)
    {
        ibv_wc wc;
        int ret = pollWithCQ(iCon_->ibcq(), 1, &wc);
        CHECK_EQ(ret, 1)
            << "** unexpected return value: should already been handled error";
    }
}

// DSM::ts_t DSM::ordered_read_sync(char *buffer,
//                                  GlobalAddress gaddr,
//                                  size_t size,
//                                  CoroContext *ctx)
// {
//     size_t dirID = get_cur_dir();
//     uint32_t rkey = remoteInfo[gaddr.nodeID].dsmRKey[dirID];
//     uint64_t wr_id = 0;
//     if (ctx)
//     {
//         wr_id = ctx->coro_id();
//         CHECK(signal);
//     }
//     c_.collect(gaddr, size, util::OpType::kRead);
//     CHECK(rdmaRead(iCon_->QPs[dirID][gaddr.nodeID],
//                    (uint64_t) buffer,
//                    remoteInfo[gaddr.nodeID].dsmBase + gaddr.offset,
//                    size,
//                    iCon_->cacheLKey,
//                    rkey,
//                    signal,
//                    wr_id));
//     if (ctx)
//     {
//         ctx->yield_to_master();
//     }
// }

// DSM::ts_t DSM::ordered_write(const char *buffer,
//                              GlobalAddress gaddr,
//                              size_t size,
//                              size_t use_qp_nr,
//                              CoroContext *ctx)
// {
//     size_t dirID = get_cur_dir();
//     uint32_t rkey = remoteInfo[gaddr.nodeID].dsmRKey[dirID];
//     uint64_t wr_id = 0;
//     auto dir_id = 0;
//     uint64_t dest = remoteInfo[gaddr.nodeID].dsmBase + gaddr.offset;

//     auto use_thcon_id = CityHash64((char *) &gaddr, sizeof(gaddr)) %
//     use_qp_nr;
//     // ordered_dist_[use_thcon_id].fetch_add(1);

//     auto &use_thcon = thCon[use_thcon_id];
//     auto &cqp = use_thcon->cQPs[dir_id][gaddr.nodeID];
//     auto lkey = use_thcon->cacheLKey;
//     cqp->write(buffer, lkey, size, dest, rkey, ctx);
//     return {};
// }

ibv_exp_send_wr *DSM::prepare_read(char *buffer,
                                   GlobalAddress gaddr,
                                   size_t size,
                                   bool on_chip,
                                   CoroContext *ctx)
{
    size_t dirID = get_cur_dir();
    uint32_t rkey = 0;
    if (on_chip)
    {
        rkey = remoteInfo[gaddr.nodeID].dmRKey[0];
    }
    else
    {
        rkey = remoteInfo[gaddr.nodeID].dsmRKey[dirID];
    }
    uint64_t dest = gaddr.offset;
    if (!on_chip)
    {
        dest += remoteInfo[gaddr.nodeID].dsmBase;
    }

    return prepare_read(buffer, gaddr.nodeID, rkey, dest, size, ctx);
}

ibv_exp_send_wr *DSM::prepare_read(char *buffer,
                                   uint32_t node_id,
                                   uint32_t rkey,
                                   uint64_t remote_addr,
                                   size_t size,
                                   CoroContext *ctx)
{
    auto &batch = get_batch(ctx);

    size_t dirID = get_cur_dir();

    c_.collect(GlobalAddress(node_id, remote_addr), size, util::OpType::kRead);

    auto &qp = iCon_->QPs[dirID][node_id];

    auto *ret = qp.prepare_read(
        (uint64_t) buffer, remote_addr, size, iCon_->cacheLKey, rkey);
    batch.add(&qp);
    return ret;
}

void DSM::write(const char *buffer,
                GlobalAddress gaddr,
                size_t size,
                bool signal,
                CoroContext *ctx)
{
    size_t dirID = get_cur_dir();
    uint32_t rkey = remoteInfo[gaddr.nodeID].dsmRKey[dirID];
    uint64_t wr_id = 0;
    if (ctx)
    {
        wr_id = ctx->coro_id();
        CHECK(signal);
    }
    c_.collect(gaddr, size, util::OpType::kWrite);

    CHECK(rdmaWrite(iCon_->QPs[dirID][gaddr.nodeID].ibqp(),
                    (uint64_t) buffer,
                    remoteInfo[gaddr.nodeID].dsmBase + gaddr.offset,
                    size,
                    iCon_->cacheLKey,
                    rkey,
                    -1,
                    signal,
                    wr_id));
    if (ctx)
    {
        ctx->yield_to_master();
    }
}

void DSM::write_sync(const char *buffer,
                     GlobalAddress gaddr,
                     size_t size,
                     CoroContext *ctx)
{
    write(buffer, gaddr, size, true, ctx);

    if (ctx == nullptr)
    {
        ibv_wc wc;
        auto ret = pollWithCQ(iCon_->ibcq(), 1, &wc);
        CHECK_EQ(ret, 1);
    }
}

ibv_exp_send_wr *DSM::prepare_write(char *buffer,
                                    GlobalAddress gaddr,
                                    size_t size,
                                    bool on_chip,
                                    CoroContext *ctx)
{
    size_t dirID = get_cur_dir();
    uint32_t rkey = 0;
    if (on_chip)
    {
        rkey = remoteInfo[gaddr.nodeID].dmRKey[0];
    }
    else
    {
        rkey = remoteInfo[gaddr.nodeID].dsmRKey[dirID];
    }
    uint64_t dest = gaddr.offset;
    if (!on_chip)
    {
        dest += remoteInfo[gaddr.nodeID].dsmBase;
    }

    return prepare_write(buffer, gaddr.nodeID, rkey, dest, size, ctx);
}

ibv_exp_send_wr *DSM::prepare_write(char *buffer,
                                    uint32_t node_id,
                                    uint32_t rkey,
                                    uint64_t remote_addr,
                                    size_t size,
                                    CoroContext *ctx)
{
    auto &batch = get_batch(ctx);
    size_t dirID = get_cur_dir();

    c_.collect(GlobalAddress(node_id, remote_addr), size, util::OpType::kWrite);

    auto &qp = iCon_->QPs[dirID][node_id];
    auto *ret = qp.prepare_write(
        remote_addr, (uint64_t) buffer, size, iCon_->cacheLKey, rkey);

    batch.add(&qp);
    return ret;
}

ibv_exp_send_wr *DSM::prepare_cas(uint32_t node_id,
                                  uint32_t rkey,
                                  uint64_t remote_addr,
                                  size_t size,
                                  uint64_t compare,
                                  uint64_t compare_mask,
                                  uint64_t swap,
                                  uint64_t swap_mask,
                                  void *rdma_buffer,
                                  CoroContext *ctx)
{
    auto &batch = get_batch(ctx);

    size_t dirID = get_cur_dir();

    c_.collect(
        GlobalAddress(node_id, (uint64_t) remote_addr), 8, util::OpType::kCas);

    auto &qp = iCon_->QPs[dirID][node_id];
    auto *ret = qp.prepare_cas(remote_addr,
                               (uint64_t) rdma_buffer,
                               size,
                               compare,
                               compare_mask,
                               swap,
                               swap_mask,
                               iCon_->cacheLKey,
                               rkey);
    batch.add(&qp);
    return ret;
}

ibv_exp_send_wr *DSM::prepare_cas(GlobalAddress gaddr,
                                  size_t size,
                                  uint64_t compare,
                                  uint64_t compare_mask,
                                  uint64_t swap,
                                  uint64_t swap_mask,
                                  void *rdma_buffer,
                                  bool on_chip,
                                  CoroContext *ctx)
{
    size_t dirID = get_cur_dir();
    uint32_t rkey = 0;
    if (on_chip)
    {
        rkey = remoteInfo[gaddr.nodeID].dmRKey[0];
    }
    else
    {
        rkey = remoteInfo[gaddr.nodeID].dsmRKey[dirID];
    }
    uint64_t dest = gaddr.offset;
    if (!on_chip)
    {
        dest += remoteInfo[gaddr.nodeID].dsmBase;
    }
    return prepare_cas(gaddr.nodeID,
                       rkey,
                       dest,
                       size,
                       compare,
                       compare_mask,
                       swap,
                       swap_mask,
                       rdma_buffer,
                       ctx);
}

ibv_exp_send_wr *DSM::prepare_faa(uint32_t node_id,
                                  uint32_t rkey,
                                  uint64_t remote_addr,
                                  size_t size,
                                  uint64_t add_val,
                                  uint64_t field_boundrary,
                                  void *rdma_buffer,
                                  CoroContext *ctx)
{
    auto &batch = get_batch(ctx);

    size_t dirID = get_cur_dir();
    c_.collect(
        GlobalAddress(node_id, (uint64_t) remote_addr), 8, util::OpType::kCas);

    auto &qp = iCon_->QPs[dirID][node_id];
    auto *ret = qp.prepare_faa(remote_addr,
                               (uint64_t) rdma_buffer,
                               size,
                               add_val,
                               field_boundrary,
                               iCon_->cacheLKey,
                               rkey);
    batch.add(&qp);
    return ret;
}

ibv_exp_send_wr *DSM::prepare_faa(GlobalAddress gaddr,
                                  size_t size,
                                  uint64_t add_val,
                                  uint64_t field_boundrary,
                                  void *rdma_buffer,
                                  bool on_chip,
                                  CoroContext *ctx)
{
    size_t dirID = get_cur_dir();
    uint32_t rkey = 0;
    if (on_chip)
    {
        rkey = remoteInfo[gaddr.nodeID].dmRKey[0];
    }
    else
    {
        rkey = remoteInfo[gaddr.nodeID].dsmRKey[dirID];
    }
    uint64_t dest = gaddr.offset;
    if (!on_chip)
    {
        dest += remoteInfo[gaddr.nodeID].dsmBase;
    }

    return prepare_faa(gaddr.nodeID,
                       rkey,
                       dest,
                       size,
                       add_val,
                       field_boundrary,
                       rdma_buffer,
                       ctx);
}

void DSM::prepare_reg_list_umr(ibv_mr *umr,
                               size_t client_nid,
                               size_t client_tid,
                               size_t dir_id,
                               ibv_exp_mem_region *mem_reg_list,
                               size_t num_mrs,
                               std::optional<uint64_t> base_addr,
                               CoroContext *ctx)
{
    auto &batch = get_batch(ctx);

    auto &qp = get_dir_cqp(client_nid, client_tid, dir_id);
    qp.prepare_reg_list_umr(umr, mem_reg_list, num_mrs, base_addr);

    batch.add(&qp);
}
void DSM::prepare_reg_repeated_umr(ibv_mr *umr,
                                   size_t client_nid,
                                   size_t client_tid,
                                   size_t dir_id,
                                   ibv_mr **mrs,
                                   size_t num_mrs,
                                   std::optional<uint64_t> base_addr,
                                   int rb_len,
                                   int rb_stride,
                                   int rb_count,
                                   CoroContext *ctx)
{
    auto &batch = get_batch(ctx);

    auto &qp = get_dir_cqp(client_nid, client_tid, dir_id);
    qp.prepare_reg_repeated_umr(
        umr, mrs, num_mrs, base_addr, rb_len, rb_stride, rb_count);

    batch.add(&qp);
}

void DSM::commit_no_wait(CoroContext *ctx)
{
    auto &batch = get_batch(ctx);

    for (auto *qp : batch.qps())
    {
        qp->commit_no_wait(ctx);
    }

    // when finished, we clear the (cached) batch
    batch.clear();
}

void DSM::commit(CoroContext *ctx, util::TraceView trace)
{
    auto &batch = get_batch(ctx);

    size_t wait_nr = 0;
    for (auto *qp : batch.qps())
    {
        bool wait = qp->commit(ctx, trace);
        wait_nr += wait;
    }

    if (wait_nr)
    {
        // wait
        if (ctx)
        {
            for (size_t i = 0; i < wait_nr; ++i)
            {
                ctx->yield_to_master();
            }
        }
        else
        {
            for (auto *qp : batch.qps())
            {
                qp->cq()->wait();
            }
        }
    }

    // when finished, we clear the (cached) batch
    batch.clear();
}

void DSM::fill_keys_dest(RdmaOpRegion &ror,
                         GlobalAddress gaddr,
                         bool is_chip,
                         size_t dirID)
{
    DCHECK_LT(dirID, NR_DIRECTORY);
    ror.lkey = iCon_->cacheLKey;
    if (is_chip)
    {
        ror.dest = remoteInfo[gaddr.nodeID].dmBase + gaddr.offset;
        ror.remoteRKey = remoteInfo[gaddr.nodeID].dmRKey[dirID];
    }
    else
    {
        ror.dest = remoteInfo[gaddr.nodeID].dsmBase + gaddr.offset;
        ror.remoteRKey = remoteInfo[gaddr.nodeID].dsmRKey[dirID];
    }
}

void DSM::write_batch(RdmaOpRegion *rs, int k, bool signal, CoroContext *ctx)
{
    int node_id = -1;
    for (int i = 0; i < k; ++i)
    {
        GlobalAddress gaddr;
        gaddr.val = rs[i].dest;
        node_id = gaddr.nodeID;
        fill_keys_dest(rs[i], gaddr, rs[i].is_on_chip);
        c_.collect(gaddr, rs[i].size, util::OpType::kWrite);
    }

    size_t cur_dir = get_cur_dir();
    if (ctx == nullptr)
    {
        rdmaWriteBatch(iCon_->QPs[cur_dir][node_id].ibqp(), rs, k, signal);
    }
    else
    {
        rdmaWriteBatch(
            iCon_->QPs[cur_dir][node_id].ibqp(), rs, k, true, ctx->coro_id());
        ctx->yield_to_master();
    }
}

void DSM::write_batch_sync(RdmaOpRegion *rs, int k, CoroContext *ctx)
{
    write_batch(rs, k, true, ctx);

    if (ctx == nullptr)
    {
        ibv_wc wc;
        pollWithCQ(iCon_->ibcq(), 1, &wc);
    }
}

void DSM::write_faa(RdmaOpRegion &write_ror,
                    RdmaOpRegion &faa_ror,
                    uint64_t add_val,
                    bool signal,
                    CoroContext *ctx)
{
    size_t cur_dir = get_cur_dir();

    int node_id;
    {
        GlobalAddress gaddr;
        gaddr.val = write_ror.dest;
        node_id = gaddr.nodeID;

        c_.collect(gaddr, write_ror.size, util::OpType::kWrite);
        fill_keys_dest(write_ror, gaddr, write_ror.is_on_chip);
    }
    {
        GlobalAddress gaddr;
        gaddr.val = faa_ror.dest;

        c_.collect(gaddr, faa_ror.size, util::OpType::kFAA);
        fill_keys_dest(faa_ror, gaddr, faa_ror.is_on_chip);
    }
    if (ctx == nullptr)
    {
        rdmaWriteFaa(iCon_->QPs[cur_dir][node_id].ibqp(),
                     write_ror,
                     faa_ror,
                     add_val,
                     signal);
    }
    else
    {
        rdmaWriteFaa(iCon_->QPs[cur_dir][node_id].ibqp(),
                     write_ror,
                     faa_ror,
                     add_val,
                     true,
                     ctx->coro_id());
        ctx->yield_to_master();
    }
}
void DSM::write_faa_sync(RdmaOpRegion &write_ror,
                         RdmaOpRegion &faa_ror,
                         uint64_t add_val,
                         CoroContext *ctx)
{
    write_faa(write_ror, faa_ror, add_val, true, ctx);
    if (ctx == nullptr)
    {
        ibv_wc wc;
        pollWithCQ(iCon_->ibcq(), 1, &wc);
    }
}

void DSM::write_cas(RdmaOpRegion &write_ror,
                    RdmaOpRegion &cas_ror,
                    uint64_t equal,
                    uint64_t val,
                    bool signal,
                    CoroContext *ctx)
{
    size_t cur_dir = get_cur_dir();

    int node_id;
    {
        GlobalAddress gaddr;
        gaddr.val = write_ror.dest;
        node_id = gaddr.nodeID;

        c_.collect(gaddr, write_ror.size, util::OpType::kWrite);
        fill_keys_dest(write_ror, gaddr, write_ror.is_on_chip);
    }
    {
        GlobalAddress gaddr;
        gaddr.val = cas_ror.dest;

        c_.collect(gaddr, cas_ror.size, util::OpType::kCas);
        fill_keys_dest(cas_ror, gaddr, cas_ror.is_on_chip);
    }
    if (ctx == nullptr)
    {
        rdmaWriteCas(iCon_->QPs[cur_dir][node_id].ibqp(),
                     write_ror,
                     cas_ror,
                     equal,
                     val,
                     signal);
    }
    else
    {
        rdmaWriteCas(iCon_->QPs[cur_dir][node_id].ibqp(),
                     write_ror,
                     cas_ror,
                     equal,
                     val,
                     true,
                     ctx->coro_id());
        ctx->yield_to_master();
    }
}
void DSM::write_cas_sync(RdmaOpRegion &write_ror,
                         RdmaOpRegion &cas_ror,
                         uint64_t equal,
                         uint64_t val,
                         CoroContext *ctx)
{
    write_cas(write_ror, cas_ror, equal, val, true, ctx);
    if (ctx == nullptr)
    {
        ibv_wc wc;
        pollWithCQ(iCon_->ibcq(), 1, &wc);
    }
}

void DSM::cas_read(RdmaOpRegion &cas_ror,
                   RdmaOpRegion &read_ror,
                   uint64_t equal,
                   uint64_t val,
                   bool signal,
                   CoroContext *ctx)
{
    size_t cur_dir = get_cur_dir();
    int node_id;
    {
        GlobalAddress gaddr;
        gaddr.val = cas_ror.dest;
        node_id = gaddr.nodeID;
        c_.collect(gaddr, cas_ror.size, util::OpType::kCas);
        fill_keys_dest(cas_ror, gaddr, cas_ror.is_on_chip);
    }
    {
        GlobalAddress gaddr;
        gaddr.val = read_ror.dest;
        c_.collect(gaddr, read_ror.size, util::OpType::kRead);
        fill_keys_dest(read_ror, gaddr, read_ror.is_on_chip);
    }

    if (ctx == nullptr)
    {
        rdmaCasRead(iCon_->QPs[cur_dir][node_id].ibqp(),
                    cas_ror,
                    read_ror,
                    equal,
                    val,
                    signal);
    }
    else
    {
        rdmaCasRead(iCon_->QPs[cur_dir][node_id].ibqp(),
                    cas_ror,
                    read_ror,
                    equal,
                    val,
                    true,
                    ctx->coro_id());
        ctx->yield_to_master();
    }
}

bool DSM::cas_read_sync(RdmaOpRegion &cas_ror,
                        RdmaOpRegion &read_ror,
                        uint64_t equal,
                        uint64_t val,
                        CoroContext *ctx)
{
    cas_read(cas_ror, read_ror, equal, val, true, ctx);

    if (ctx == nullptr)
    {
        ibv_wc wc;
        pollWithCQ(iCon_->ibcq(), 1, &wc);
    }

    return equal == *(uint64_t *) cas_ror.source;
}

void DSM::cas(GlobalAddress gaddr,
              uint64_t equal,
              uint64_t val,
              uint64_t *rdma_buffer,
              bool signal,
              CoroContext *ctx)
{
    size_t cur_dir = get_cur_dir();
    uint64_t wr_id = 0;
    if (ctx)
    {
        wr_id = ctx->coro_id();
        CHECK(signal);
    }
    c_.collect(gaddr, 8, util::OpType::kCas);
    CHECK(rdmaCompareAndSwap(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                             (uint64_t) rdma_buffer,
                             remoteInfo[gaddr.nodeID].dsmBase + gaddr.offset,
                             equal,
                             val,
                             iCon_->cacheLKey,
                             remoteInfo[gaddr.nodeID].dsmRKey[cur_dir],
                             signal,
                             wr_id));
    if (ctx)
    {
        ctx->yield_to_master();
    }
}

bool DSM::cas_sync(GlobalAddress gaddr,
                   uint64_t equal,
                   uint64_t val,
                   uint64_t *rdma_buffer,
                   CoroContext *ctx)
{
    cas(gaddr, equal, val, rdma_buffer, true, ctx);

    if (ctx == nullptr)
    {
        ibv_wc wc;
        auto ret = pollWithCQ(iCon_->ibcq(), 1, &wc);
        CHECK_EQ(ret, 1);
    }

    return equal == *rdma_buffer;
}

void DSM::cas_mask(GlobalAddress gaddr,
                   uint64_t equal,
                   uint64_t val,
                   uint64_t *rdma_buffer,
                   uint64_t mask,
                   bool signal)
{
    size_t cur_dir = get_cur_dir();
    c_.collect(gaddr, 8, util::OpType::kCas);
    rdmaCompareAndSwapMask(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                           (uint64_t) rdma_buffer,
                           remoteInfo[gaddr.nodeID].dsmBase + gaddr.offset,
                           equal,
                           val,
                           iCon_->cacheLKey,
                           remoteInfo[gaddr.nodeID].dsmRKey[cur_dir],
                           mask,
                           signal);
}

bool DSM::cas_mask_sync(GlobalAddress gaddr,
                        uint64_t equal,
                        uint64_t val,
                        uint64_t *rdma_buffer,
                        uint64_t mask)
{
    cas_mask(gaddr, equal, val, rdma_buffer, mask);
    ibv_wc wc;
    pollWithCQ(iCon_->ibcq(), 1, &wc);

    return (equal & mask) == (*rdma_buffer & mask);
}

void DSM::faa_boundary(GlobalAddress gaddr,
                       uint64_t add_val,
                       uint64_t *rdma_buffer,
                       uint64_t mask,
                       bool signal,
                       CoroContext *ctx)
{
    size_t cur_dir = get_cur_dir();
    c_.collect(gaddr, 8, util::OpType::kFAA);
    if (ctx == nullptr)
    {
        rdmaFetchAndAddBoundary(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                                (uint64_t) rdma_buffer,
                                remoteInfo[gaddr.nodeID].dsmBase + gaddr.offset,
                                add_val,
                                iCon_->cacheLKey,
                                remoteInfo[gaddr.nodeID].dsmRKey[cur_dir],
                                mask,
                                signal);
    }
    else
    {
        rdmaFetchAndAddBoundary(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                                (uint64_t) rdma_buffer,
                                remoteInfo[gaddr.nodeID].dsmBase + gaddr.offset,
                                add_val,
                                iCon_->cacheLKey,
                                remoteInfo[gaddr.nodeID].dsmRKey[cur_dir],
                                mask,
                                true,
                                ctx->coro_id());
        ctx->yield_to_master();
    }
}
void DSM::faa_boundary_sync(GlobalAddress gaddr,
                            uint64_t add_val,
                            uint64_t *rdma_buffer,
                            uint64_t mask,
                            CoroContext *ctx)
{
    faa_boundary(gaddr, add_val, rdma_buffer, mask, true, ctx);
    if (ctx == nullptr)
    {
        ibv_wc wc;
        pollWithCQ(iCon_->ibcq(), 1, &wc);
    }
}

void DSM::read_dm(char *buffer,
                  GlobalAddress gaddr,
                  size_t size,
                  bool signal,
                  CoroContext *ctx)
{
    // only dirID == 0 has dm

    size_t cur_dir = 0;
    c_.collect(gaddr, size, util::OpType::kDMRead);
    if (ctx == nullptr)
    {
        rdmaRead(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                 (uint64_t) buffer,
                 remoteInfo[gaddr.nodeID].dmBase + gaddr.offset,
                 size,
                 iCon_->cacheLKey,
                 remoteInfo[gaddr.nodeID].dmRKey[cur_dir],
                 signal);
    }
    else
    {
        rdmaRead(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                 (uint64_t) buffer,
                 remoteInfo[gaddr.nodeID].dmBase + gaddr.offset,
                 size,
                 iCon_->cacheLKey,
                 remoteInfo[gaddr.nodeID].dmRKey[cur_dir],
                 true,
                 ctx->coro_id());
        ctx->yield_to_master();
    }
}

void DSM::read_dm_sync(char *buffer,
                       GlobalAddress gaddr,
                       size_t size,
                       CoroContext *ctx)
{
    read_dm(buffer, gaddr, size, true, ctx);

    if (ctx == nullptr)
    {
        ibv_wc wc;
        pollWithCQ(iCon_->ibcq(), 1, &wc);
    }
}

void DSM::write_dm(const char *buffer,
                   GlobalAddress gaddr,
                   size_t size,
                   bool signal,
                   CoroContext *ctx)
{
    size_t cur_dir = 0;
    c_.collect(gaddr, size, util::OpType::kDMWrite);
    if (ctx == nullptr)
    {
        rdmaWrite(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                  (uint64_t) buffer,
                  remoteInfo[gaddr.nodeID].dmBase + gaddr.offset,
                  size,
                  iCon_->cacheLKey,
                  remoteInfo[gaddr.nodeID].dmRKey[cur_dir],
                  -1,
                  signal);
    }
    else
    {
        rdmaWrite(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                  (uint64_t) buffer,
                  remoteInfo[gaddr.nodeID].dmBase + gaddr.offset,
                  size,
                  iCon_->cacheLKey,
                  remoteInfo[gaddr.nodeID].dmRKey[cur_dir],
                  -1,
                  true,
                  ctx->coro_id());
        ctx->yield_to_master();
    }
}

void DSM::write_dm_sync(const char *buffer,
                        GlobalAddress gaddr,
                        size_t size,
                        CoroContext *ctx)
{
    write_dm(buffer, gaddr, size, true, ctx);

    if (ctx == nullptr)
    {
        ibv_wc wc;
        pollWithCQ(iCon_->ibcq(), 1, &wc);
    }
}

void DSM::cas_dm(GlobalAddress gaddr,
                 uint64_t equal,
                 uint64_t val,
                 uint64_t *rdma_buffer,
                 bool signal,
                 CoroContext *ctx)
{
    size_t cur_dir = 0;
    c_.collect(gaddr, 8, util::OpType::kDMCas);
    if (ctx == nullptr)
    {
        rdmaCompareAndSwap(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                           (uint64_t) rdma_buffer,
                           remoteInfo[gaddr.nodeID].dmBase + gaddr.offset,
                           equal,
                           val,
                           iCon_->cacheLKey,
                           remoteInfo[gaddr.nodeID].dmRKey[cur_dir],
                           signal);
    }
    else
    {
        rdmaCompareAndSwap(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                           (uint64_t) rdma_buffer,
                           remoteInfo[gaddr.nodeID].dmBase + gaddr.offset,
                           equal,
                           val,
                           iCon_->cacheLKey,
                           remoteInfo[gaddr.nodeID].dmRKey[cur_dir],
                           true,
                           ctx->coro_id());
        ctx->yield_to_master();
    }
}

bool DSM::cas_dm_sync(GlobalAddress gaddr,
                      uint64_t equal,
                      uint64_t val,
                      uint64_t *rdma_buffer,
                      CoroContext *ctx)
{
    cas_dm(gaddr, equal, val, rdma_buffer, true, ctx);

    if (ctx == nullptr)
    {
        ibv_wc wc;
        pollWithCQ(iCon_->ibcq(), 1, &wc);
    }

    return equal == *rdma_buffer;
}

void DSM::cas_dm_mask(GlobalAddress gaddr,
                      uint64_t equal,
                      uint64_t val,
                      uint64_t *rdma_buffer,
                      uint64_t mask,
                      bool signal)
{
    size_t cur_dir = 0;
    c_.collect(gaddr, 8, util::OpType::kDMCas);
    CHECK(rdmaCompareAndSwapMask(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                                 (uint64_t) rdma_buffer,
                                 remoteInfo[gaddr.nodeID].dmBase + gaddr.offset,
                                 equal,
                                 val,
                                 iCon_->cacheLKey,
                                 remoteInfo[gaddr.nodeID].dmRKey[cur_dir],
                                 mask,
                                 signal));
}

bool DSM::cas_dm_mask_sync(GlobalAddress gaddr,
                           uint64_t equal,
                           uint64_t val,
                           uint64_t *rdma_buffer,
                           uint64_t mask)
{
    cas_dm_mask(gaddr, equal, val, rdma_buffer, mask);
    ibv_wc wc;
    pollWithCQ(iCon_->ibcq(), 1, &wc);

    return (equal & mask) == (*rdma_buffer & mask);
}

void DSM::faa_dm_boundary(GlobalAddress gaddr,
                          uint64_t add_val,
                          uint64_t *rdma_buffer,
                          uint64_t mask,
                          bool signal,
                          CoroContext *ctx)
{
    size_t cur_dir = 0;
    c_.collect(gaddr, 8, util::OpType::kDMFAA);
    if (ctx == nullptr)
    {
        rdmaFetchAndAddBoundary(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                                (uint64_t) rdma_buffer,
                                remoteInfo[gaddr.nodeID].dmBase + gaddr.offset,
                                add_val,
                                iCon_->cacheLKey,
                                remoteInfo[gaddr.nodeID].dmRKey[cur_dir],
                                mask,
                                signal);
    }
    else
    {
        rdmaFetchAndAddBoundary(iCon_->QPs[cur_dir][gaddr.nodeID].ibqp(),
                                (uint64_t) rdma_buffer,
                                remoteInfo[gaddr.nodeID].dmBase + gaddr.offset,
                                add_val,
                                iCon_->cacheLKey,
                                remoteInfo[gaddr.nodeID].dmRKey[cur_dir],
                                mask,
                                true,
                                ctx->coro_id());
        ctx->yield_to_master();
    }
}

void DSM::faa_dm_boundary_sync(GlobalAddress gaddr,
                               uint64_t add_val,
                               uint64_t *rdma_buffer,
                               uint64_t mask,
                               CoroContext *ctx)
{
    faa_dm_boundary(gaddr, add_val, rdma_buffer, mask, true, ctx);
    if (ctx == nullptr)
    {
        ibv_wc wc;
        pollWithCQ(iCon_->ibcq(), 1, &wc);
    }
}

ibv_mw *DSM::alloc_mw(size_t dirID)
{
    DCHECK_LT(dirID, dirCon.size());
    auto *ctx = &dirCon[dirID]->ctx;
    // dinfo("[dsm] dirCon ID: %d, pd: %p", dirCon[cur_dir].dirID, ctx->pd);
    struct ibv_mw *mw = ibv_alloc_mw(ctx->pd, ctx->mw_type);
    if (!mw)
    {
        PLOG(ERROR) << "failed to create memory window.";
    }
    // dinfo("allocating mw at pd: %p, type: %d", ctx->pd, ctx->mw_type);
    return mw;
}

void DSM::free_mw(struct ibv_mw *mw)
{
    PCHECK(ibv_dealloc_mw(mw) == 0) << "failed to destroy mw";
}

bool DSM::bind_memory_region(struct ibv_mw *mw,
                             size_t target_node_id,
                             size_t target_thread_id,
                             const char *buffer,
                             size_t size,
                             size_t dirID,
                             size_t wr_id,
                             bool signal)
{
    DCHECK_LT(dirID, dirCon.size());
    auto *qp = get_dir_qp(target_node_id, target_thread_id, dirID);
    uint32_t rkey = rdmaAsyncBindMemoryWindow(
        qp, mw, dirCon[dirID]->dsmMR, (uint64_t) buffer, size, signal, wr_id);
    return rkey != 0;
}
bool DSM::bind_memory_region_sync(struct ibv_mw *mw,
                                  size_t target_node_id,
                                  size_t target_thread_id,
                                  const char *buffer,
                                  size_t size,
                                  size_t dirID,
                                  uint64_t wr_id,
                                  CoroContext *ctx)
{
    DCHECK_LT(dirID, dirCon.size());
    auto *qp = get_dir_qp(target_node_id, target_thread_id, dirID);
    uint32_t rkey = rdmaAsyncBindMemoryWindow(
        qp, mw, dirCon[dirID]->dsmMR, (uint64_t) buffer, size, true, wr_id);
    if (rkey == 0)
    {
        return false;
    }
    if (unlikely(ctx == nullptr))
    {
        struct ibv_wc wc;
        int ret = pollWithCQ(dirCon[dirID]->ibcq(), 1, &wc) == 1;
        if (ret < 0)
        {
            rdmaQueryQueuePair(qp);
            return false;
        }
        return true;
    }
    else
    {
        ctx->yield_to_master();
    }
    return true;
}

ibv_mr *DSM::create_umr(size_t dir_id, size_t klm_size)
{
    auto *ctx = get_dir_rdma_context(dir_id);
    auto *pd = ctx->pd;

    struct ibv_exp_create_mr_in mrin;
    memset(&mrin, 0, sizeof(mrin));
    mrin.pd = pd;
    // current driver requires this flag on.
    mrin.attr.create_flags = IBV_EXP_MR_INDIRECT_KLMS;
    mrin.attr.exp_access_flags =
        IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_REMOTE_WRITE |
        IBV_EXP_ACCESS_REMOTE_READ | IBV_EXP_ACCESS_REMOTE_ATOMIC;
    mrin.attr.max_klm_list_size = klm_size;
    ibv_mr *umr = ibv_exp_create_mr(&mrin);
    if (unlikely(umr == nullptr))
    {
        PLOG(ERROR) << "Failed to create UMR";
    }
    return umr;
}

void DSM::destroy_mr(ibv_mr *mr)
{
    if (likely(mr != nullptr))
    {
        int ret = ibv_dereg_mr(mr);
        PLOG_IF(FATAL, ret != 0) << "** failed to dereg mr: " << PRE(*mr);
    }
}

mem::SecondaryAllocator::Pointer DSM::get_secondary_allocator()
{
    return std::make_shared<DSMSecondaryAllocator>(this);
}

int DSM::try_master_coro_poll(CoroContext *mctx, size_t limit)
{
    DCHECK(DCHECK_NOTNULL(mctx)->is_master());

    auto *cq = iCon_->cq();

    int ret = cq->try_wait(limit, [mctx](const ibv_exp_wc &wc) {
        auto *wr_ctx = (rdma::WRCtx *) wc.wr_id;
        if (wr_ctx->ctx_)
        {
            mctx->yield_to_worker(wr_ctx->ctx_->coro_id());
            //    std::ignore = mctx;
        }
        else
        {
            LOG(WARNING) << PRE(wr_ctx) << " has no coroutine context.";
        }
    });
    return ret;
}