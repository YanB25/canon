#include "Worker.h"

#include "DSM.h"
#include "GlobalAllocator.h"
#include "RPC.h"
#include "memory/block_allocator.h"

using namespace rpc;

// NOTE: dsm_addr starts from 0.
Worker::Worker(DSM *dsm,
               size_t wid,
               void *dsm_base_addr,
               ::mem::BaseAllocator::pointer alloc)
    : dsm_(dsm), wid_(wid), dsm_base_addr_(dsm_base_addr), alloc_(alloc)
{
}

void Worker::work()
{
    LOG(INFO) << "worker " << wid_ << " launched.";
    dsm_->registerThread();

    auto from_ep = kServerStartEpId + wid_;
    while (!exited_.load(std::memory_order_relaxed))
    {
        DSM::msg_desc_t msg_desc[32];
        auto ret = dsm_->unreliable_try_recv_no_cpy_from(from_ep, msg_desc, 32);
        for (size_t i = 0; i < ret; ++i)
        {
            const char *buf = msg_desc[i].msg_addr;
            auto &req = *(Request *) buf;

            switch (req.hdr.type)
            {
            case RPCType::kAlloc:
            {
                handle_request_alloc((const AllocRequest &) req);
                break;
            }
            default:
            {
                LOG(FATAL) << "Unknown RPC type " << (int) req.hdr.type;
            }
            }

            dsm_->return_buf_no_cpy_from(from_ep, &msg_desc[i], 1);
        }
    }
    LOG(INFO) << "worker " << wid_ << " exiting...";
}

void Worker::handle_request_alloc(const AllocRequest &req)
{
    auto resp_buf = dsm_->get_rdma_buffer(sizeof(AllocResponse));
    auto &resp = *(AllocResponse *) resp_buf.buffer;

    resp.hdr = req.hdr;

    if (unlikely(req.flags & (flag_t) mem::AllocFlag::kMock))
    {
        resp.addr = 0xaabbccdd11223344;
    }
    else
    {
        char *ret = (char *) alloc_->alloc(req.size);
        if (unlikely(ret == nullptr))
        {
            resp.addr = 0;
        }
        else
        {
            DCHECK_GE(ret, dsm_base_addr_);
            auto offset = ret - (char *) dsm_base_addr_;
            resp.addr = offset;
        }
    }

    dsm_->unreliable_send(resp_buf.buffer,
                          sizeof(AllocResponse),
                          req.hdr.from_nid,
                          req.hdr.from_epid);

    dsm_->put_rdma_buffer(std::move(resp_buf));
}