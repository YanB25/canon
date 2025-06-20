#pragma once

#include <cinttypes>

#include "./provider.h"
#include "DSM.h"
#include "GlobalAddress.h"

namespace avis
{
class AvisManager
{
public:
    using Provider = avis::BuddyProvider;
    AvisManager(DSM::pointer dsm,
                size_t buddy_nr,
                std::optional<size_t> pub_size_opt,
                std::optional<size_t> arc_client_nr)
        : dsm_(dsm), buddy_nr_(buddy_nr)
    {
        uint32_t nid = dsm->get_node_id();
        for (size_t i = 0; i < buddy_nr_; ++i)
        {
            // This is HashTable, so allocate DSM from me.
            auto meta_size = 2_MB;
            auto meta = dsm_->alloc_from(meta_size, nid, 4_KB);
            CHECK(!meta.is_null());
            auto data_size = 2_GB;
            auto data = dsm_->alloc_from(data_size, nid, 4_KB);
            CHECK(!data.is_null());
            providers_.push_back(Provider{.node_id = nid,
                                          .meta_raddr = meta,
                                          .meta_size = 2_MB,
                                          .buf_raddr = data,
                                          .buf_size = 2_GB,
                                          .page_size = 4_KB});
        }
        dsm_->put("size", (uint32_t) buddy_nr, 100ms);
        dsm_->put("providers",
                  providers_.data(),
                  providers_.size() * sizeof(Provider),
                  100ms);

        if (pub_size_opt)
        {
            pub_size_ = *pub_size_opt;
            pub_meta_ = dsm_->alloc_from(pub_size_, nid);
            auto rdma_buf = dsm_->get_rdma_buffer(pub_size_);
            memset(rdma_buf.buffer, 0, pub_size_);
            dsm_->prepare_write(
                rdma_buf.buffer, pub_meta_, pub_size_, false, nullptr);
            dsm_->commit();
            dsm_->put_rdma_buffer(std::move(rdma_buf));
            dsm_->put("pub", pub_meta_, 100ms);
            dsm_->put("pub_size", pub_size_, 100ms);
        }

        if (arc_client_nr)
        {
            auto nr = *arc_client_nr;
            auto arc_size = nr * nr * sizeof(uint64_t);
            auto arc_meta = dsm_->alloc_from(arc_size, nid, 8);
            auto arc_buf = dsm_->get_rdma_buffer(arc_size);
            memset(arc_buf.buffer, 0, arc_size);
            dsm_->prepare_write(
                arc_buf.buffer, arc_meta, arc_size, false, nullptr);
            dsm_->commit();
            dsm_->put_rdma_buffer(std::move(arc_buf));
            dsm_->put("arc_meta", arc_meta, 100ms);
            dsm_->put("arc_size", (size_t) arc_size, 100ms);
        }
    }
    const auto &providers() const
    {
        return providers_;
    }

private:
    DSM::pointer dsm_;
    size_t buddy_nr_;

    std::vector<Provider> providers_;

    size_t pub_size_{};
    GlobalAddress pub_meta_{};
};

}  // namespace avis