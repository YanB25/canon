#include "sherman/Tree.h"

#include <city.h>

#include <algorithm>
#include <iostream>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

#include "Common.h"
#include "CoroContext.h"
#include "GlobalAddress.h"
#include "RdmaBuffer.h"
#include "Timer.h"
#include "avis/avis.h"
#include "avis/usage.h"
#include "sherman/IndexCache.h"
#include "sherman/TreeConfig.h"
#include "util/Coro.h"
#include "util/ProcessMem.h"

namespace sherman
{
bool enter_debug = false;

#define LATENCY_WINDOWS 1000000

// for store root pointer

uint64_t latency[kMaxAppThread][LATENCY_WINDOWS]{};

thread_local CoroCall Tree::worker[define::kMaxCoroNr];
thread_local CoroCall Tree::master;
thread_local GlobalAddress path_stack[define::kMaxCoroNr]
                                     [define::kMaxLevelOfTree];
thread_local bool Tree::coro_finished[define::kMaxCoroNr];
thread_local size_t Tree::coro_finished_nr{0};

thread_local Timer timer;
thread_local std::queue<uint16_t> Tree::hot_wait_queue;

thread_local std::shared_ptr<avis::AvisHandle> avis_handle[define::kMaxCoroNr];
thread_local std::shared_ptr<avis::AvisHandle> master_avis_handle;

thread_local AllocCache Tree::alloc_cache_[define::kMaxCoroNr];

Tree::Tree(std::shared_ptr<DSM> dsm,
           GlobalAddress tree_meta,
           const TreeConfig &conf,
           size_t server_nid)
    : dsm(dsm), tree_meta_(tree_meta), c_(conf), server_nid_(server_nid)
{
    // must perform "reset" at bootstrap
    maybe_reset_page_cache(conf.bucket_nr, conf.cache_limit);

    for (int i = 0; i < dsm->getClusterSize(); ++i)
    {
        local_locks[i] = new LocalLockNode[define::kNumOfLock];
        for (size_t k = 0; k < define::kNumOfLock; ++k)
        {
            auto &n = local_locks[i][k];
            n.ticket_lock.store(0);
            n.hand_over = false;
            n.hand_time = 0;
        }
    }

    CHECK(dsm->is_register());
    print_verbose();

    index_cache = new IndexCache(define::kIndexCacheSize);

    root_ptr_ptr = get_root_ptr_ptr();

    // try to init tree and install root pointer
    auto page = dsm->get_rdma_page(kInternalPageSize);
    auto *page_buffer = page.data();
    auto root_addr = do_alloc(kLeafPageSize, nullptr);
    avis::Usage::ins().collect(kLeafPageSize);
    // DCHECK(util::is_aligned(root_addr.offset, kInternalPageSize));

    auto root_page = new (page_buffer) LeafPage;

    root_page->set_consistent();
    dsm->prepare_write(page_buffer, root_addr, kLeafPageSize, false, nullptr);
    dsm->commit(nullptr);

    if (c_.is_local)
    {
        // write through
        page_cache().write_aligned(
            std::move(page), root_addr, kLeafPageSize, nullptr);
        page_buffer = nullptr;
    }

    auto cas_buffer = (dsm->get_rbuf(0)).get_cas_buffer();
    dsm->prepare_cas(root_ptr_ptr,
                     8,
                     0,
                     0xffffffffffffffff,
                     root_addr.val,
                     0xffffffffffffffff,
                     cas_buffer,
                     false,
                     nullptr);
    dsm->commit(nullptr);
    uint64_t old_val = *(uint64_t *) cas_buffer;
    if (old_val == 0)
    {
        LOG(WARNING) << "Tree root pointer value update to " << root_addr
                     << std::endl;
        // We can't do CAS on local page cache
        // just invalidate it
        if (c_.is_local)
        {
            GlobalAddress page_gaddr = root_ptr_ptr;
            page_gaddr.offset =
                util::to_aligned(page_gaddr.offset, kInternalPageSize);
            page_cache().invalidate(page_gaddr, nullptr);
        }
    }
    else
    {
        // std::cout << "fail\n";
    }
}
Tree::~Tree()
{
    for (int i = 0; i < dsm->getClusterSize(); ++i)
    {
        delete[] local_locks[i];
    }
    delete index_cache;
}

void Tree::print_verbose()
{
    int kLeafHdrOffset = offsetof(LeafPage, hdr);
    int kInternalHdrOffset = offsetof(InternalPage, hdr);
    if (kLeafHdrOffset != kInternalHdrOffset)
    {
        LOG(WARNING) << "format error";
    }

    if (dsm->getMyNodeID() == 0)
    {
        std::cout << "Header size: " << sizeof(Header) << std::endl;
        std::cout << "Internal Page size: " << sizeof(InternalPage) << " ["
                  << kInternalPageSize << "]" << std::endl;
        std::cout << "Internal per Page: " << kInternalCardinality << std::endl;
        std::cout << "Leaf Page size: " << sizeof(LeafPage) << " ["
                  << kLeafPageSize << "]" << std::endl;
        std::cout << "Leaf per Page: " << kLeafCardinality << std::endl;
        std::cout << "LeafEntry size: " << sizeof(LeafEntry) << std::endl;
        std::cout << "InternalEntry size: " << sizeof(InternalEntry)
                  << std::endl;
    }
}

void Tree::register_avis_handle(CoroContext *ctx)
{
    CHECK(c_.avis_.has_value());
    auto ptl = std::make_shared<avis::PTL>(dsm, c_.avis_->server_nid, ctx);
    auto pub = std::make_shared<avis::Publisher<avis::BitmapPub>>(
        dsm, c_.avis_->pub_meta, c_.avis_->pub_size, ctx);
    auto handle = std::make_shared<avis::AvisHandle>(
        c_.avis_->providers, dsm, ptl, pub, ctx);
    if (ctx)
    {
        avis_handle[ctx->coro_id()] = handle;
    }
    else
    {
        master_avis_handle = handle;
    }
}
void Tree::reset_avis_handle(CoroContext *ctx)
{
    if (ctx)
    {
        avis_handle[ctx->coro_id()].reset();
    }
    else
    {
        master_avis_handle.reset();
    }
}

inline void Tree::before_operation(CoroContext *, int coro_id)
{
    for (size_t i = 0; i < define::kMaxLevelOfTree; ++i)
    {
        path_stack[coro_id][i] = GlobalAddress::Null();
    }
}

GlobalAddress Tree::get_root_ptr_ptr()
{
    return tree_meta_;
}

extern GlobalAddress g_root_ptr;
extern int g_root_level;
extern bool enable_cache;
GlobalAddress Tree::get_root_ptr(CoroContext *cxt, int coro_id)
{
    if (g_root_ptr == GlobalAddress::Null())
    {
        auto page_buffer = (dsm->get_rbuf(coro_id)).get_page_buffer();
        // skip this 8B read ptr in the page cache
        dsm->prepare_read(
            page_buffer, root_ptr_ptr, sizeof(GlobalAddress), false, cxt);
        dsm->commit(cxt);
        GlobalAddress root_ptr = *(GlobalAddress *) page_buffer;

        // CHECK(util::is_aligned(root_ptr.offset, kInternalPageSize));
        // LOG(INFO) << "[debug] root_ptr is " << PRE(root_ptr);
        CHECK(!root_ptr.is_null());
        return root_ptr;
    }
    else
    {
        // CHECK(util::is_aligned(g_root_ptr.offset, kInternalPageSize));
        // LOG(INFO) << "[debug] root_ptr is " << PRE(g_root_ptr);
        CHECK(!g_root_ptr.is_null());
        return g_root_ptr;
    }

    // std::cout << "root ptr " << root_ptr << std::endl;
}

void Tree::broadcast_new_root(GlobalAddress new_root_addr, int root_level)
{
    RawMessage m;
    m.type = RpcType::NEW_ROOT;
    m.addr = new_root_addr;
    m.level = root_level;
    for (int i = 0; i < dsm->getClusterSize(); ++i)
    {
        dsm->rpc_call_dir(m, i);
    }
}

bool Tree::update_new_root(GlobalAddress left,
                           const Key &k,
                           GlobalAddress right,
                           int level,
                           GlobalAddress old_root,
                           CoroContext *cxt,
                           int coro_id)
{
    auto page = dsm->get_rdma_page(kInternalPageSize);
    char *page_buffer = page.data();
    auto cas_buffer = dsm->get_rbuf(coro_id).get_cas_buffer();
    auto new_root = new (page_buffer) InternalPage(left, k, right, level);

    auto new_root_addr = fast_alloc(kInternalPageSize, cxt);
    avis::Usage::ins().collect(kInternalPageSize);
    // DCHECK(util::is_aligned(new_root_addr.offset, kInternalPageSize));

    new_root->set_consistent();
    if (c_.is_local)
    {
        page_cache().write_aligned(
            std::move(page), new_root_addr, kInternalPageSize, cxt);
        page_buffer = nullptr;
    }
    else
    {
        dsm->prepare_write(
            page_buffer, new_root_addr, kInternalPageSize, false, cxt);
        dsm->commit(cxt);
    }
    dsm->prepare_cas(root_ptr_ptr,
                     8,
                     old_root.val,
                     0xffffffffffffffff,
                     new_root_addr.val,
                     0xffffffffffffffff,
                     cas_buffer,
                     false,
                     cxt);
    dsm->commit(cxt);
    uint64_t got_val = *(uint64_t *) cas_buffer;
    if (got_val == old_root.val)
    {
        if (c_.is_local)
        {
            page_cache().invalidate(root_ptr_ptr, cxt);
        }
        broadcast_new_root(new_root_addr, level);
        std::cout << "new root level " << level << " " << new_root_addr
                  << std::endl;
        return true;
    }
    else
    {
        std::cout << "cas root fail " << std::endl;
    }

    return false;
}

void Tree::print_path_stack(Key key, CoroContext *cxt, int coro_id)
{
    static std::mutex mu_;
    std::lock_guard<std::mutex> lk(mu_);
    LOG(INFO) << "======= path_stack for " << PRE(key) << " ===========";
    LOG(INFO) << PRE(get_root_ptr(cxt, coro_id));

    auto page_buffer = (dsm->get_rbuf(coro_id)).get_page_buffer();
    for (ssize_t level = define::kMaxLevelOfTree; level >= 0; --level)
    {
        auto p = path_stack[coro_id][level];
        if (p.is_null())
        {
            LOG(INFO) << "Skipping level " << level << ": nullptr";
            continue;
        }
        dsm->prepare_read(page_buffer, p, kLeafPageSize, false, cxt);
        dsm->commit(cxt);

        auto header = (Header *) (page_buffer + (offsetof(LeafPage, hdr)));
        GlobalAddress expect_next_gaddr;
        GlobalAddress actual_next_gaddr;
        if (level - 1 >= 0)
        {
            actual_next_gaddr = path_stack[coro_id][level - 1];
        }

        LOG(INFO) << "Page[" << level << "] " << (header->is_leaf() ? "L" : "I")
                  << " @ " << p << "=> " << std::endl
                  << "level: " << int(header->level)
                  << ", lowest key: " << header->lowest
                  << ", highest: " << header->highest;

        if (header->is_internal())
        {
            auto internal_page = (InternalPage *) page_buffer;
            auto lowest = header->lowest;
            auto highest = header->highest;
            LOG_IF(WARNING, key < lowest)
                << "** Wrong: " << PRE(key) << " < " << PRE(lowest);
            LOG_IF(WARNING, key > highest)
                << "** Wrong: " << PRE(key) << " > " << PRE(highest);

            auto first_key = internal_page->records[0].key;

            if (actual_next_gaddr == header->leftmost_ptr)
            {
                LOG(INFO) << "* actually goes to: left_most";
            }

            if (key < first_key)
            {
                expect_next_gaddr = header->leftmost_ptr;
                LOG(INFO) << PRE(key) << " < " << PRE(first_key)
                          << ": goto left_most: " << expect_next_gaddr;
            }
            else
            {
                // for finding expect ones
                for (ssize_t i = header->last_index; i >= 0; --i)
                {
                    auto that_key = internal_page->records[i].key;
                    if (key >= that_key)
                    {
                        expect_next_gaddr = internal_page->records[i].ptr;
                        LOG(INFO) << PRE(key) << " >= " << PRE(that_key)
                                  << ": goto record[" << i
                                  << "]: " << expect_next_gaddr;
                        break;
                    }
                }
                for (ssize_t i = header->last_index; i >= 0; --i)
                {
                    auto that_ptr = internal_page->records[i].ptr;
                    if (!that_ptr.is_null() && that_ptr == actual_next_gaddr)
                    {
                        LOG(INFO)
                            << "** actually goes to record[" << i
                            << "] with key " << internal_page->records[i].key;
                    }
                }
            }
        }
        else
        {
            // is leaf
            auto leaf_page = (LeafPage *) page_buffer;
            bool found = false;
            for (ssize_t i = header->last_index; i >= 0; --i)
            {
                auto that_key = leaf_page->records[i].key;
                if (key == that_key)
                {
                    LOG(INFO)
                        << "HIT: " << PRE(key)
                        << ", value: " << PRE(leaf_page->records[i].value);
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                LOG(INFO) << "MISS: " << PRE(key);
            }
        }

        LOG_IF(WARNING, expect_next_gaddr.is_null())
            << "Don't know where to go.";
        if (level - 1 >= 0)
        {
            auto actual = path_stack[coro_id][level - 1];
            LOG_IF(WARNING, expect_next_gaddr != actual)
                << "** Path is wrong. Expect: " << expect_next_gaddr << " vs "
                << actual;
        }
    }
}

std::pair<size_t, size_t> Tree::print_and_check_tree(CoroContext *cxt)
{
    size_t level_nr = 0;
    size_t node_nr = 0;
    int coro_id = cxt ? cxt->coro_id() : 0;
    CHECK(dsm->is_register());

    auto root = get_root_ptr(cxt, coro_id);
    // SearchResult result;

    GlobalAddress p = root;
    GlobalAddress levels[define::kMaxLevelOfTree];
    int level_cnt = 0;
    auto page_buffer = (dsm->get_rbuf(coro_id)).get_page_buffer();
    GlobalAddress leaf_head;

next_level:
    level_nr++;
    node_nr++;

    dsm->prepare_read(page_buffer, p, kLeafPageSize, false, cxt);
    dsm->commit(cxt);

    auto header = (Header *) (page_buffer + (offsetof(LeafPage, hdr)));
    levels[level_cnt++] = p;
    LOG(INFO) << "[header @" << p << "] " << *header;
    if (header->level != 0)
    {
        p = header->leftmost_ptr;
        goto next_level;
    }
    else
    {
        leaf_head = p;
    }

next:
    node_nr++;
    dsm->prepare_read(page_buffer, leaf_head, kLeafPageSize, false, cxt);
    dsm->commit(cxt);

    auto page = (LeafPage *) page_buffer;
    // LOG(INFO) << "[leaf @" << leaf_head << "] "
    //           << pre_leaf_page(*page, false /* verbose */);

    while (page->hdr.sibling_ptr != GlobalAddress::Null())
    {
        leaf_head = page->hdr.sibling_ptr;
        goto next;
    }

    // for (int i = 0; i < level_cnt; ++i) {
    //   dsm->read_sync(page_buffer, levels[i], kLeafPageSize);
    //   auto header = (Header *)(page_buffer + (STRUCT_OFFSET(LeafPage, hdr)));
    //   // std::cout << "addr: " << levels[i] << " ";
    //   // header->debug();
    //   // std::cout << " | ";
    //   while (header->sibling_ptr != GlobalAddress::Null()) {
    //     dsm->read_sync(page_buffer, header->sibling_ptr, kLeafPageSize);
    //     header = (Header *)(page_buffer + (STRUCT_OFFSET(LeafPage, hdr)));
    //     // std::cout << "addr: " << header->sibling_ptr << " ";
    //     // header->debug();
    //     // std::cout << " | ";
    //   }
    //   // std::cout << "\n------------------------------------" << std::endl;
    //   // std::cout << "------------------------------------" << std::endl;
    // }
    return {level_nr, node_nr};
}

GlobalAddress Tree::query_cache(const Key &)
{
    return GlobalAddress::Null();
}

inline bool Tree::try_lock_addr(GlobalAddress lock_addr,
                                uint64_t tag,
                                uint64_t *buf,
                                CoroContext *cxt,
                                int coro_id)
{
    // VLOG(V) << "[Tree] locking " << (void *) lock_addr.val
    //         << " with tag: " << (void *) tag << ". addr detail: " <<
    //         lock_addr;
    if (c_.is_local)
    {
        page_cache().try_lock_addr(lock_addr, tag, buf, cxt, coro_id);
        return true;
    }
    else
    {
        bool hand_over = acquire_local_lock(lock_addr, cxt, coro_id);
        if (hand_over)
        {
            return true;
        }

        {
            uint64_t retry_cnt = 0;
            uint64_t pre_tag = 0;
            uint64_t conflict_tag = 0;
        retry:
            retry_cnt++;
            if (retry_cnt && retry_cnt % 1000000 == 0)
            {
                LOG(ERROR) << "Deadlock at " << lock_addr << std::endl
                           << "node: " << dsm->get_node_id()
                           << ", thread: " << dsm->get_thread_id()
                           << ", lock by " << (conflict_tag >> 32) << ", "
                           << (conflict_tag << 32 >> 32);
            }

            dsm->prepare_cas(lock_addr,
                             8,
                             0,
                             0xffffffffffffffff,
                             tag,
                             0xffffffffffffffff,
                             buf,
                             true,
                             cxt);
            dsm->commit(cxt);
            uint64_t got_val = *(uint64_t *) buf;
            bool res = got_val == 0;
            if (!res)
            {
                conflict_tag = *buf - 1;
                if (conflict_tag != pre_tag)
                {
                    retry_cnt = 0;
                    pre_tag = conflict_tag;
                }
                goto retry;
            }
            else
            {
                // LOG(INFO) << "[Tree] locked. " << lock_addr;
            }
        }

        return true;
    }
}

inline void Tree::unlock_addr(GlobalAddress lock_addr,
                              uint64_t tag,
                              uint64_t *,
                              CoroContext *cxt,
                              int coro_id,
                              bool async)
{
    VLOG(V) << "[Tree] unlocking " << (void *) lock_addr.val
            << " with tag: " << (void *) tag << ". addr_detail: " << lock_addr;
    if (c_.is_local)
    {
        page_cache().unlock_addr(lock_addr, tag, cxt, coro_id, async);
    }
    else
    {
        bool hand_over_other = can_hand_over(lock_addr);
        if (hand_over_other)
        {
            releases_local_lock(lock_addr);
            return;
        }

        auto cas_buf = dsm->get_rbuf(coro_id).get_cas_buffer();

        *cas_buf = 0;

        // TODO: may have overhead here
        dsm->prepare_write((char *) cas_buf,
                           lock_addr,
                           sizeof(uint64_t),
                           true /* on chip */,
                           cxt);
        if (async)
        {
            dsm->commit(cxt);
        }
        else
        {
            dsm->commit(cxt);
            // dsm->commit(cxt);
        }

        releases_local_lock(lock_addr);
    }
}

/**
 * Local Buffer: local_page[offset:offset + size]
 * Remote Page: page_gaddr[offset:offset + size]
 * @param page page-aligned.
 * @param page_gaddr page-aligned.
 */
void Tree::write_page_and_unlock(util::Page &&page,
                                 GlobalAddress page_gaddr,
                                 off_t page_offset,
                                 size_t size,
                                 [[maybe_unused]] uint64_t *cas_buffer,
                                 GlobalAddress lock_addr,
                                 [[maybe_unused]] uint64_t tag,
                                 CoroContext *cxt,
                                 int coro_id,
                                 bool async)
{
    if (likely(c_.is_local))
    {
        page_cache().write_page_and_unlock(std::move(page),
                                           page_gaddr,
                                           page_offset,
                                           size,
                                           cas_buffer,
                                           lock_addr,
                                           tag,
                                           cxt,
                                           coro_id,
                                           async);
    }
    else
    {
        auto target_gaddr = page_gaddr;
        target_gaddr.offset += page_offset;
        bool hand_over_other = can_hand_over(lock_addr);
        if (hand_over_other)
        {
            char *page_buffer = page.data() + page_offset;
            DCHECK(page.is_DMA());
            dsm->prepare_write(page_buffer, target_gaddr, size, false, cxt);
            dsm->commit(cxt);
            releases_local_lock(lock_addr);
            return;
        }

        dsm->prepare_write(
            page.data() + page_offset, target_gaddr, size, false, cxt);
        char *cas_buf = (char *) dsm->get_rbuf(coro_id).get_cas_buffer();
        *((uint64_t *) cas_buf) = 0;
        dsm->prepare_write(cas_buf, lock_addr, sizeof(uint64_t), true, cxt);

        if (async)
        {
            dsm->commit(cxt);
        }
        else
        {
            // dsm->commit(cxt);
            dsm->commit(cxt);
        }

        releases_local_lock(lock_addr);
    }
}

std::shared_ptr<util::Page> Tree::lock_and_read_page_no_modify(
    GlobalAddress page_addr,
    int page_size,
    uint64_t *cas_buffer,
    GlobalAddress lock_addr,
    uint64_t tag,
    CoroContext *cxt,
    int coro_id)
{
    if (likely(c_.is_local))
    {
        return page_cache().lock_and_read_page_no_modify(
            page_addr, page_size, cas_buffer, lock_addr, tag, cxt, coro_id);
    }
    else
    {
        auto page = dsm->get_rdma_page(page_size);
        auto page_ptr = std::make_shared<util::Page>(std::move(page));
        try_lock_addr(lock_addr, tag, cas_buffer, cxt, coro_id);

        dsm->prepare_read(page_ptr->data(), page_addr, page_size, false, cxt);
        dsm->commit(cxt);
        return page_ptr;
    }
}

void Tree::lock_bench(const Key &k, CoroContext *cxt, int coro_id)
{
    uint64_t lock_index =
        CityHash64((char *) &k, sizeof(k)) % define::kNumOfLock;

    GlobalAddress lock_addr;
    lock_addr.nodeID = 0;
    lock_addr.offset = lock_index * sizeof(uint64_t);
    auto cas_buffer = dsm->get_rbuf(coro_id).get_cas_buffer();

    try_lock_addr(lock_addr, 1, cas_buffer, cxt, coro_id);
    unlock_addr(lock_addr, 1, cas_buffer, cxt, coro_id, true);
}

void Tree::insert_internal(
    const Key &k, GlobalAddress v, CoroContext *cxt, int coro_id, int level)
{
    auto root = get_root_ptr(cxt, coro_id);
    SearchResult result;

    GlobalAddress p = root;

next:
    // DCHECK(util::is_aligned(p.offset, kInternalPageSize));

    if (!page_search(p, k, result, cxt, coro_id))
    {
        LOG(WARNING) << "SEARCH WARNING insert";
        p = get_root_ptr(cxt, coro_id);
        sleep(1);
        goto next;
    }

    assert(result.level != 0);
    if (result.slibing != GlobalAddress::Null())
    {
        p = result.slibing;
        goto next;
    }

    if (result.level >= level + 1)
    {
        p = result.next_level;
        if (result.level > level + 1)
        {
            goto next;
        }
    }
    // p = result.next_level;
    // if (result.level != level + 1)
    // {
    //     goto next;
    // }

    // DCHECK(util::is_aligned(p.offset, kInternalPageSize));
    DCHECK(result.level == level + 1 || result.level == level)
        << fmt::format("result.level: {}, level: {}", result.level, level);
    internal_page_store(p, k, v, root, level, cxt, coro_id);
}

void Tree::insert(const Key &k, const Value &v, CoroContext *cxt)
{
    CHECK_NE(v, Value{}) << "** Invalid inserting null. got: " << v;
    auto coro_id = cxt ? cxt->coro_id() : 0;

    DCHECK(dsm->is_register());

    before_operation(cxt, coro_id);

    if (enable_cache)
    {
        GlobalAddress cache_addr;
        auto entry = index_cache->search_from_cache(
            k, &cache_addr, dsm->getMyThreadID() == 0);
        if (entry)
        {  // cache hit
            auto root = get_root_ptr(cxt, coro_id);
            // DCHECK(util::is_aligned(root.offset, kInternalPageSize));

            if (leaf_page_store(cache_addr, k, v, root, 0, cxt, coro_id, true))
            {
                cache_hit_.current()++;
                return;
            }
            // cache stale, from root,
            index_cache->invalidate(entry);
        }
        cache_miss_.current()++;
    }

    auto root = get_root_ptr(cxt, coro_id);
    SearchResult result;

    GlobalAddress p = root;

next:
    // DCHECK(util::is_aligned(p.offset, kInternalPageSize));

    if (!page_search(p, k, result, cxt, coro_id))
    {
        std::cout << "SEARCH WARNING insert" << std::endl;
        p = get_root_ptr(cxt, coro_id);
        sleep(1);
        goto next;
    }

    if (!result.is_leaf)
    {
        assert(result.level != 0);
        if (result.slibing != GlobalAddress::Null())
        {
            p = result.slibing;
            goto next;
        }

        p = result.next_level;
        if (result.level != 1)
        {
            goto next;
        }
    }
    leaf_page_store(p, k, v, root, 0, cxt, coro_id);
}

bool Tree::search(const Key &k, Value &v, CoroContext *cxt)
{
    assert(dsm->is_register());
    auto coro_id = cxt ? cxt->coro_id() : 0;

    auto root = get_root_ptr(cxt, coro_id);
    SearchResult result;

    GlobalAddress p = root;

    bool from_cache = false;
    const CacheEntry *entry = nullptr;
    if (enable_cache)
    {
        GlobalAddress cache_addr;
        entry = index_cache->search_from_cache(
            k, &cache_addr, dsm->getMyThreadID() == 0);
        if (entry)
        {  // cache hit
            cache_hit_.current()++;
            from_cache = true;
            p = cache_addr;
        }
        else
        {
            cache_miss_.current()++;
        }
    }

next:
    if (!page_search(p, k, result, cxt, coro_id, from_cache))
    {
        if (from_cache)
        {  // cache stale
            index_cache->invalidate(entry);
            cache_hit_.current()--;
            cache_miss_.current()++;
            from_cache = false;

            p = root;
        }
        else
        {
            std::cout << "SEARCH WARNING search" << std::endl;
            sleep(1);
        }
        goto next;
    }
    if (result.is_leaf)
    {
        if (result.val != kValueNull)
        {  // find
            v = result.val;
            return true;
        }
        if (result.slibing != GlobalAddress::Null())
        {  // turn right
            p = result.slibing;
            goto next;
        }
        return false;  // not found
    }
    else
    {  // internal
        p = result.slibing != GlobalAddress::Null() ? result.slibing
                                                    : result.next_level;
        goto next;
    }
}

void Tree::del(const Key &k, CoroContext *cxt)
{
    DCHECK(dsm->is_register());
    auto coro_id = cxt ? cxt->coro_id() : 0;

    before_operation(cxt, coro_id);

    if (enable_cache)
    {
        GlobalAddress cache_addr;
        auto entry = index_cache->search_from_cache(
            k, &cache_addr, dsm->getMyThreadID() == 0);
        if (entry)
        {  // cache hit
            if (leaf_page_del(cache_addr, k, 0, cxt, coro_id, true))
            {
                cache_hit_.current()++;
                return;
            }
            // cache stale, from root,
            index_cache->invalidate(entry);
        }
        cache_miss_.current()++;
    }

    auto root = get_root_ptr(cxt, coro_id);
    SearchResult result;

    GlobalAddress p = root;

next:

    if (!page_search(p, k, result, cxt, coro_id))
    {
        std::cout << "SEARCH WARNING del" << std::endl;
        p = get_root_ptr(cxt, coro_id);
        sleep(1);
        goto next;
    }

    if (!result.is_leaf)
    {
        assert(result.level != 0);
        if (result.slibing != GlobalAddress::Null())
        {
            p = result.slibing;
            goto next;
        }

        p = result.next_level;
        if (result.level != 1)
        {
            goto next;
        }
    }

    leaf_page_del(p, k, 0, cxt, coro_id);
}

bool Tree::page_search(GlobalAddress page_addr,
                       const Key &k,
                       SearchResult &result,
                       CoroContext *cxt,
                       int coro_id,
                       bool from_cache)
{
    // DCHECK(util::is_aligned(page_addr.offset, kInternalPageSize));

    std::shared_ptr<util::Page> page_guard;  // Page lifetime determines here
    int counter = 0;
re_read:
    counter++;
    if (c_.is_local)
    {
        page_guard =
            page_cache().read_aligned_no_modify(page_addr, kLeafPageSize, cxt);
    }
    else
    {
        auto page = dsm->get_rdma_page(kLeafPageSize);
        dsm->prepare_read(page.data(), page_addr, kLeafPageSize, false, cxt);
        dsm->commit(cxt);
        page_guard = std::make_shared<util::Page>(std::move(page));
    }
    const char *page_buffer = page_guard.get()->data();
    const auto *header =
        (const Header *) (page_buffer + (offsetof(LeafPage, hdr)));

    memset(&result, 0, sizeof(result));
    result.is_leaf = header->leftmost_ptr == GlobalAddress::Null();
    result.level = header->level;
    path_stack[coro_id][result.level] = page_addr;
    // std::cout << "level " << (int)result.level << " " << page_addr <<
    // std::endl;

    if (result.is_leaf)
    {
        const auto *page = (const LeafPage *) page_buffer;
        if (!page->check_consistent())
        {
            LOG_IF(FATAL, counter > 100)
                << "** re-read too many times: leaf page constantly not "
                   "consistent: "
                << PRE(page->version()) << page_addr << ", key: " << k
                << ", result: " << PRE(result)
                << ", from_cache: " << from_cache;
            goto re_read;
        }

        if (from_cache && (k < page->hdr.lowest || k >= page->hdr.highest))
        {  // cache is stale
            return false;
        }

        assert(result.level == 0);
        if (k >= page->hdr.highest)
        {  // should turn right
            result.slibing = page->hdr.sibling_ptr;
            return true;
        }
        if (k < page->hdr.lowest)
        {
            assert(false);
            return false;
        }
        leaf_page_search(page, k, result);
    }
    else
    {
        assert(result.level != 0);
        assert(!from_cache);
        auto page = (const InternalPage *) page_buffer;

        if (!page->check_consistent())
        {
            LOG_IF(FATAL, counter > 100)
                << "** re-read too many times: internal page constantly not "
                   "consistent: "
                << PRE(page->version()) << page_addr << ", key: " << k
                << ", result: " << PRE(result)
                << ", from_cache: " << from_cache;
            goto re_read;
        }

        if (result.level == 1 && enable_cache)
        {
            index_cache->add_to_cache(page);
        }

        if (k >= page->hdr.highest)
        {  // should turn right
            result.slibing = page->hdr.sibling_ptr;
            return true;
        }
        if (k < page->hdr.lowest)
        {
            // print_and_check_tree(cxt, coro_id);
            print_path_stack(k, cxt, coro_id);
            LOG(FATAL) << "key " << k << " error in level "
                       << int(page->hdr.level)
                       << ". from_cache: " << from_cache;
            return false;
        }
        internal_page_search(page, k, result);
    }

    return true;
}

void Tree::internal_page_search(const InternalPage *page,
                                const Key &k,
                                SearchResult &result)
{
    assert(k >= page->hdr.lowest);
    assert(k < page->hdr.highest);

    auto cnt = page->hdr.last_index + 1;
    // page->debug();
    if (k < page->records[0].key)
    {
        result.next_level = page->hdr.leftmost_ptr;
        return;
    }

    for (int i = 1; i < cnt; ++i)
    {
        if (k < page->records[i].key)
        {
            result.next_level = page->records[i - 1].ptr;
            return;
        }
    }
    result.next_level = page->records[cnt - 1].ptr;
}

void Tree::leaf_page_search(const LeafPage *page,
                            const Key &k,
                            SearchResult &result)
{
    for (int i = 0; i < kLeafCardinality; ++i)
    {
        auto &r = page->records[i];
        if (r.key == k && r.value != kValueNull &&
            r.f_version().load() == r.r_version().load())
        {
            result.val = r.value;
            break;
        }
    }
}

void Tree::internal_page_store(GlobalAddress page_addr,
                               const Key &k,
                               GlobalAddress v,
                               GlobalAddress root,
                               int level,
                               CoroContext *cxt,
                               int coro_id)
{
    uint64_t lock_index =
        CityHash64((char *) &page_addr, sizeof(page_addr)) % define::kNumOfLock;

    GlobalAddress lock_addr;
    lock_addr.nodeID = page_addr.nodeID;
    lock_addr.offset = lock_index * sizeof(uint64_t);

    auto &rbuf = dsm->get_rbuf(coro_id);
    uint64_t *cas_buffer = rbuf.get_cas_buffer();

    auto tag = dsm->getThreadTag();
    DCHECK_NE(tag, 0);

    auto ro_page_ptr = lock_and_read_page_no_modify(
        page_addr, kInternalPageSize, cas_buffer, lock_addr, tag, cxt, coro_id);

    const auto *ro_page = (const InternalPage *) ro_page_ptr.get()->data();

    DCHECK_EQ(ro_page->hdr.level, level);
    DCHECK(ro_page->check_consistent());
    if (k >= ro_page->hdr.highest)
    {
        this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);

        assert(ro_page->hdr.sibling_ptr != GlobalAddress::Null());

        this->internal_page_store(
            ro_page->hdr.sibling_ptr, k, v, root, level, cxt, coro_id);

        return;
    }
    // when reach here, page will be modified
    // Do copy-on-write here.
    util::Page new_page = *ro_page_ptr;  // COPY
    auto *page = (InternalPage *) new_page.data();
    assert(k >= page->hdr.lowest);

    auto cnt = page->hdr.last_index + 1;

    bool is_update = false;
    uint16_t insert_index = 0;
    for (int i = cnt - 1; i >= 0; --i)
    {
        if (page->records[i].key == k)
        {  // find and update
            page->records[i].ptr = v;
            // assert(false);
            is_update = true;
            break;
        }
        if (page->records[i].key < k)
        {
            insert_index = i + 1;
            break;
        }
    }

    assert(cnt != kInternalCardinality);

    if (!is_update)
    {  // insert and shift
        for (int i = cnt; i > insert_index; --i)
        {
            page->records[i].key = page->records[i - 1].key;
            page->records[i].ptr = page->records[i - 1].ptr;
        }
        page->records[insert_index].key = k;
        page->records[insert_index].ptr = v;

        page->hdr.last_index++;
    }

    cnt = page->hdr.last_index + 1;
    bool need_split = cnt == kInternalCardinality;
    Key split_key;
    GlobalAddress sibling_addr;
    if (need_split)
    {
        // need split
        avis::Usage::ins().collect(kInternalPageSize);
        sibling_addr = fast_alloc(kInternalPageSize, cxt);
        // DCHECK(util::is_aligned(sibling_addr.offset, kInternalPageSize));
        auto sibling_page = dsm->get_rdma_page(kInternalPageSize);
        auto *sibling_buf = sibling_page.data();

        auto *sibling = new (sibling_buf) InternalPage(page->hdr.level);

        //    std::cout << "addr " <<  sibling_addr << " | level " <<
        //    (int)(page->hdr.level) << std::endl;

        int m = cnt / 2;
        split_key = page->records[m].key;
        assert(split_key > page->hdr.lowest);
        assert(split_key < page->hdr.highest);
        for (int i = m + 1; i < cnt; ++i)
        {  // move
            sibling->records[i - m - 1].key = page->records[i].key;
            sibling->records[i - m - 1].ptr = page->records[i].ptr;
        }
        if (avis::Config::ins().enable_bp())
        {
            // LOG(INFO) << "[BP] wait for internal";
            std::this_thread::sleep_for(80us);
        }
        page->hdr.last_index -= (cnt - m);
        sibling->hdr.last_index += (cnt - m - 1);

        sibling->hdr.leftmost_ptr = page->records[m].ptr;
        sibling->hdr.lowest = page->records[m].key;
        sibling->hdr.highest = page->hdr.highest;
        page->hdr.highest = page->records[m].key;

        // link
        sibling->hdr.sibling_ptr = page->hdr.sibling_ptr;
        page->hdr.sibling_ptr = sibling_addr;

        sibling->set_consistent();
        if (likely(c_.is_local))
        {
            page_cache().write_aligned(
                std::move(sibling_page), sibling_addr, kInternalPageSize, cxt);
        }
        else
        {
            dsm->prepare_write(
                sibling_buf, sibling_addr, kInternalPageSize, false, cxt);
            dsm->commit(cxt);
        }
    }

    page->set_consistent();
    write_page_and_unlock(std::move(new_page),
                          page_addr,
                          0 /* offset */,
                          kInternalPageSize,
                          cas_buffer,
                          lock_addr,
                          tag,
                          cxt,
                          coro_id,
                          need_split);

    if (!need_split)
        return;

    if (root == page_addr)
    {  // update root

        if (update_new_root(page_addr,
                            split_key,
                            sibling_addr,
                            level + 1,
                            root,
                            cxt,
                            coro_id))
        {
            return;
        }
    }

    auto up_level = path_stack[coro_id][level + 1];

    if (up_level != GlobalAddress::Null())
    {
        internal_page_store(
            up_level, split_key, sibling_addr, root, level + 1, cxt, coro_id);
    }
    else
    {
        LOG(FATAL) << "up_level: " << up_level
                   << " is GlobalAddress::Null. path_stack[" << coro_id << "]["
                   << level + 1 << "]";
    }
}

bool Tree::leaf_page_store(GlobalAddress page_addr,
                           const Key &k,
                           const Value &v,
                           GlobalAddress root,
                           int level,
                           CoroContext *cxt,
                           int coro_id,
                           bool from_cache)
{
    // DCHECK(util::is_aligned(page_addr.offset, kInternalPageSize));
    uint64_t lock_index =
        CityHash64((char *) &page_addr, sizeof(page_addr)) % define::kNumOfLock;

    GlobalAddress lock_addr;

#ifdef CONFIG_ENABLE_EMBEDDING_LOCK
    lock_addr = page_addr;
#else
    lock_addr.nodeID = page_addr.nodeID;
    lock_addr.offset = lock_index * sizeof(uint64_t);
#endif

    auto &rbuf = dsm->get_rbuf(coro_id);
    uint64_t *cas_buffer = rbuf.get_cas_buffer();

    auto tag = dsm->getThreadTag();
    DCHECK_NE(tag, 0);

    auto ro_page_ptr = lock_and_read_page_no_modify(
        page_addr, kLeafPageSize, cas_buffer, lock_addr, tag, cxt, coro_id);

    const auto *ro_page = (const LeafPage *) ro_page_ptr.get()->data();

    DCHECK_EQ(ro_page->hdr.level, level);
    DCHECK(ro_page->check_consistent());

    if (from_cache && (k < ro_page->hdr.lowest || k >= ro_page->hdr.highest))
    {  // cache is stale
        this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
        return false;
    }

    if (k >= ro_page->hdr.highest)
    {
        this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
        assert(ro_page->hdr.sibling_ptr != GlobalAddress::Null());
        this->leaf_page_store(
            ro_page->hdr.sibling_ptr, k, v, root, level, cxt, coro_id);
        return true;
    }
    // when reach here, will modify the page
    // perform CoW here
    util::Page new_page = *ro_page_ptr;  // COPY
    auto *page = (LeafPage *) new_page.data();
    DCHECK_GE(k, page->hdr.lowest)
        << "page: " << new_page << ". from cache: " << from_cache;

    int cnt = 0;
    int empty_index = -1;
    char *update_addr = nullptr;
    for (int i = 0; i < kLeafCardinality; ++i)
    {
        auto &r = page->records[i];
        if (r.value != kValueNull)
        {
            cnt++;
            if (r.key == k)
            {
                // freeing the old
                GlobalAddress to_free_gaddr((void *) r.value);
                auto value_size_buf = dsm->get_rdma_buffer(sizeof(uint64_t));
                dsm->prepare_read(value_size_buf.buffer,
                                  to_free_gaddr,
                                  sizeof(uint64_t),
                                  false,
                                  cxt);
                dsm->commit(cxt);
                uint64_t size = *(uint64_t *) value_size_buf.buffer;
                dsm->put_rdma_buffer(std::move(value_size_buf));

                if (avis::Config::ins().enable_ptl())
                {
                    auto *handle = get_avis_handle(cxt);
                    handle->ptl()->record_free(to_free_gaddr);
                }

                fast_free(to_free_gaddr, size, cxt);

                r.value = v;
                r.f_version()++;
                r.r_version().store(r.f_version().load());
                update_addr = (char *) &r;
                break;
            }
        }
        else if (empty_index == -1)
        {
            empty_index = i;
        }
    }

    DCHECK_NE(cnt, kLeafCardinality);

    if (update_addr == nullptr)
    {
        // insert new item
        if (empty_index == -1)
        {
            printf("%d cnt\n", cnt);
            assert(false);
        }

        auto &r = page->records[empty_index];
        r.key = k;
        r.value = v;
        r.f_version()++;
        r.r_version().store(r.f_version().load());

        update_addr = (char *) &r;

        cnt++;
    }

    bool need_split = cnt == kLeafCardinality;
    if (!need_split)
    {
        DCHECK(update_addr);
        off_t offset = (update_addr - (char *) page);
        write_page_and_unlock(std::move(new_page),
                              page_addr,
                              offset,
                              sizeof(LeafEntry),
                              cas_buffer,
                              lock_addr,
                              tag,
                              cxt,
                              coro_id,
                              false);

        return true;
    }
    else
    {
        std::sort(page->records,
                  page->records + kLeafCardinality,
                  [](const LeafEntry &a, const LeafEntry &b)
                  { return a.key < b.key; });
    }

    Key split_key;
    GlobalAddress sibling_addr;
    if (need_split)
    {
        // need split
        avis::Usage::ins().collect(kLeafPageSize);
        sibling_addr = fast_alloc(kLeafPageSize, cxt);
        // DCHECK(util::is_aligned(sibling_addr.offset, kInternalPageSize));

        auto sibling_page = dsm->get_rdma_page(kInternalPageSize);
        char *sibling_buf = sibling_page.data();

        auto sibling = new (sibling_buf) LeafPage(page->hdr.level);

        // std::cout << "addr " <<  sibling_addr << " | level " <<
        // (int)(page->hdr.level) << std::endl;

        int m = cnt / 2;
        split_key = page->records[m].key;
        assert(split_key > page->hdr.lowest);
        assert(split_key < page->hdr.highest);

        for (int i = m; i < cnt; ++i)
        {  // move
            sibling->records[i - m].key = page->records[i].key;
            sibling->records[i - m].value = page->records[i].value;
            page->records[i].key = 0;
            page->records[i].value = kValueNull;
        }
        if (avis::Config::ins().enable_bp())
        {
            // LOG(INFO) << "[BP] wait for leaf split";
            std::this_thread::sleep_for(80us);
        }

        page->hdr.last_index -= (cnt - m);
        sibling->hdr.last_index += (cnt - m);

        sibling->hdr.lowest = split_key;
        sibling->hdr.highest = page->hdr.highest;
        page->hdr.highest = split_key;

        // link
        sibling->hdr.sibling_ptr = page->hdr.sibling_ptr;
        page->hdr.sibling_ptr = sibling_addr;

        sibling->set_consistent();
        if (likely(c_.is_local))
        {
            page_cache().write_aligned(
                std::move(sibling_page), sibling_addr, kLeafPageSize, cxt);
        }
        else
        {
            dsm->prepare_write(
                sibling_buf, sibling_addr, kLeafPageSize, false, cxt);
            dsm->commit(cxt);
        }
    }

    page->set_consistent();

    write_page_and_unlock(std::move(new_page),
                          page_addr,
                          0 /* offset */,
                          kLeafPageSize,
                          cas_buffer,
                          lock_addr,
                          tag,
                          cxt,
                          coro_id,
                          need_split);

    if (!need_split)
        return true;

    if (root == page_addr)
    {  // update root
        if (update_new_root(page_addr,
                            split_key,
                            sibling_addr,
                            level + 1,
                            root,
                            cxt,
                            coro_id))
        {
            return true;
        }
    }

    auto up_level = path_stack[coro_id][level + 1];

    if (up_level != GlobalAddress::Null())
    {
        internal_page_store(
            up_level, split_key, sibling_addr, root, level + 1, cxt, coro_id);
    }
    else
    {
        assert(from_cache);
        insert_internal(split_key, sibling_addr, cxt, coro_id, level + 1);
    }

    return true;
}

bool Tree::leaf_page_del(GlobalAddress page_addr,
                         const Key &k,
                         int level,
                         CoroContext *cxt,
                         int coro_id,
                         bool from_cache)
{
    uint64_t lock_index =
        CityHash64((char *) &page_addr, sizeof(page_addr)) % define::kNumOfLock;

    GlobalAddress lock_addr;

#ifdef CONFIG_ENABLE_EMBEDDING_LOCK
    lock_addr = page_addr;
#else
    lock_addr.nodeID = page_addr.nodeID;
    lock_addr.offset = lock_index * sizeof(uint64_t);
#endif

    auto &rbuf = dsm->get_rbuf(coro_id);
    uint64_t *cas_buffer = rbuf.get_cas_buffer();

    auto tag = dsm->getThreadTag();
    assert(tag != 0);

    auto ro_page_ptr = lock_and_read_page_no_modify(
        page_addr, kLeafPageSize, cas_buffer, lock_addr, tag, cxt, coro_id);

    const auto *ro_page = (const LeafPage *) ro_page_ptr.get()->data();

    assert(ro_page->hdr.level == level);
    assert(ro_page->check_consistent());

    if (from_cache && (k < ro_page->hdr.lowest || k >= ro_page->hdr.highest))
    {
        // cache is stale
        this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
        return false;
    }

    if (k >= ro_page->hdr.highest)
    {
        this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, true);
        assert(ro_page->hdr.sibling_ptr != GlobalAddress::Null());
        this->leaf_page_del(ro_page->hdr.sibling_ptr, k, level, cxt, coro_id);
        return true;
    }

    // reach here, need to modify the page
    // perform CoW
    auto new_page = *ro_page_ptr;  // COPY
    auto *page = (LeafPage *) new_page.data();

    assert(k >= page->hdr.lowest);

    char *update_addr = nullptr;
    for (int i = 0; i < kLeafCardinality; ++i)
    {
        auto &r = page->records[i];
        if (r.key == k && r.value != kValueNull)
        {
            GlobalAddress to_free_gaddr((void *) r.value);
            auto value_size_buf = dsm->get_rdma_buffer(sizeof(uint64_t));
            dsm->prepare_read(value_size_buf.buffer,
                              to_free_gaddr,
                              sizeof(uint64_t),
                              false,
                              cxt);
            dsm->commit(cxt);
            uint64_t size = *(uint64_t *) value_size_buf.buffer;
            dsm->put_rdma_buffer(std::move(value_size_buf));

            if (avis::Config::ins().enable_ptl())
            {
                auto *handle = get_avis_handle(cxt);
                handle->ptl()->record_free(to_free_gaddr);
            }

            fast_free(to_free_gaddr, size, cxt);

            DCHECK(!avis::Config::ins().enable_bp())
                << "TODO: too hard, I don't know what to do";

            r.value = kValueNull;
            r.f_version()++;
            r.r_version().store(r.f_version().load());
            update_addr = (char *) &r;
            break;
        }
    }

    if (update_addr)
    {
        off_t offset = (update_addr - (char *) page);
        write_page_and_unlock(std::move(new_page),
                              page_addr,
                              offset,
                              sizeof(LeafEntry),
                              cas_buffer,
                              lock_addr,
                              tag,
                              cxt,
                              coro_id,
                              false);
    }
    else
    {
        this->unlock_addr(lock_addr, tag, cas_buffer, cxt, coro_id, false);
    }
    return true;
}

void Tree::run_coroutine(CoroFunc func,
                         int id,
                         int coro_cnt,
                         bool is_master,
                         ::bench::StopToken::pointer stop_token)
{
    for (int i = 0; i < (int) define::kMaxCoroNr; ++i)
    {
        if (i < coro_cnt)
        {
            coro_finished[i] = false;
        }
        else
        {
            coro_finished[i] = true;
        }
    }
    coro_finished_nr = 0;

    auto cb = CoroControlBlock::make_ptr();
    DCHECK_LE(coro_cnt, define::kMaxCoroNr);
    for (int i = 0; i < coro_cnt; ++i)
    {
        auto *gen = func(i, dsm.get(), id);

        worker[i] = CoroCall(
            [gen, coro_id = i, stop_token, is_master, cb = cb.get(), this](
                CoroYield &yield)
            {
                auto tid = util::get_thread_id();
                CoroContext ctx(tid, &yield, &master, coro_id, cb);
                this->coro_worker(gen, ctx, is_master, stop_token);
            });
    }

    master = CoroCall(
        [coro_cnt, stop_token, is_master, cb = cb.get(), this](CoroYield &yield)
        {
            auto tid = util::get_thread_id();
            CoroContext ctx(tid, &yield, worker, cb);
            this->coro_master(ctx, coro_cnt, is_master, stop_token);
        });

    master();
}

void Tree::run_coroutine_lock_bench(CoroFunc func,
                                    int id,
                                    int coro_cnt,
                                    bool is_master,
                                    ::bench::StopToken::pointer stop_token)
{
    for (int i = 0; i < (int) define::kMaxCoroNr; ++i)
    {
        if (i < coro_cnt)
        {
            coro_finished[i] = false;
        }
        else
        {
            coro_finished[i] = true;
        }
    }
    coro_finished_nr = 0;

    DCHECK_LE(coro_cnt, define::kMaxCoroNr);
    auto cb = CoroControlBlock::make_ptr();
    for (int i = 0; i < coro_cnt; ++i)
    {
        auto *gen = func(i, dsm.get(), id);
        worker[i] = CoroCall(
            [gen, stop_token, is_master, cb = cb.get(), this](CoroYield &yield)
            {
                auto tid = util::get_thread_id();
                CoroContext ctx(tid, &yield, worker, cb);
                this->coro_worker_bench_lock(gen, ctx, is_master, stop_token);
            });
    }

    master = CoroCall(
        [coro_cnt, stop_token, is_master, cb = cb.get(), this](CoroYield &yield)
        {
            auto tid = util::get_thread_id();
            CoroContext ctx(tid, &yield, worker, cb);
            this->coro_master(ctx, coro_cnt, is_master, stop_token);
        });

    master();
}

void Tree::coro_worker(RequstGen *gen,
                       CoroContext &ctx,
                       bool is_master,
                       StopToken::pointer stop_token)
{
    ChronoTimer chrono_timer;
    auto coro_id = ctx.coro_id();

    auto min = std::chrono::nanoseconds(0ns).count();
    auto max = std::chrono::nanoseconds(20ms).count();
    auto step = std::chrono::nanoseconds(1us).count();
    OnePassBucketMonitor<uint64_t> lat_m(min, max, step);

    while (likely(!stop_token->stop_requested()))
    {
        auto r = gen->next();

        if (is_master)
        {
            chrono_timer.pin();
        }

        if (r.is_search)
        {
            Value v;
            this->search(r.k, v, &ctx);
        }
        else
        {
            auto value_size = r.value_size;
            avis::Usage::ins().collect(value_size);
            GlobalAddress value_raddr = do_alloc(value_size, &ctx);
            Buffer rdma_buf = dsm->get_rdma_buffer(value_size);
            memcpy(rdma_buf.buffer, &value_size, sizeof(value_size));
            dsm->prepare_write(
                rdma_buf.buffer, value_raddr, sizeof(uint64_t), false, &ctx);
            // NOTE: no need to commit, just go on
            r.v = value_raddr.val;

            prepare_alloc(&ctx);

            this->insert(r.k, r.v, &ctx);

            dsm->put_rdma_buffer(std::move(rdma_buf));
        }

        if (is_master)
        {
            auto ns = chrono_timer.pin();
            stop_token->collect_ns(ns);
            lat_m.collect(ns);
        }

        stop_token->complete_task(1);
    }

    CHECK(stop_token->stop_requested());
    CHECK(!coro_finished[coro_id]);
    coro_finished[coro_id] = true;
    coro_finished_nr += 1;

    LOG_IF(INFO, !lat_m.empty()) << PRE(lat_m);

    if (is_master)
    {
        auto *handle = get_avis_handle(&ctx);
        if (handle)
        {
            LOG(INFO) << "PTL induced RDMA: " << handle->ptl()->metric();
            LOG(INFO) << "BP induced RDMA: " << handle->ptl()->bp_metric();
            LOG(INFO) << "In cache: " << handle->dump();
            for (auto &buddy : handle->get_buddys())
            {
                for (const auto &[order, lat] : buddy.second->dump_latency())
                {
                    LOG(INFO) << "Buddy(" << buddy.first << ") order " << order
                              << ": " << lat;
                }
            }
        }
    }

    ctx.record_yield_reason(true /* exit */);
    ctx.yield_to_master();

    __builtin_unreachable();
}

void Tree::coro_worker_bench_lock(RequstGen *gen,
                                  CoroContext &ctx,
                                  bool is_master,
                                  StopToken::pointer stop_token)
{
    ChronoTimer chrono_timer;
    auto coro_id = ctx.coro_id();

    while (true)
    {
        auto r = gen->next();

        if (is_master)
        {
            chrono_timer.pin();
        }

        this->lock_bench(r.k, &ctx, coro_id);

        if (is_master)
        {
            auto ns = chrono_timer.pin();
            stop_token->collect_ns(ns);
        }

        stop_token->complete_task(1);
        if (unlikely(stop_token->stop_requested()))
        {
            CHECK(!coro_finished[coro_id]);
            coro_finished[coro_id] = true;
            coro_finished_nr += 1;

            ctx.record_yield_reason(true /* exit */);
            ctx.yield_to_master();

            __builtin_unreachable();
        }
    }
}

void Tree::coro_master(CoroContext &mctx,
                       int coro_cnt,
                       [[maybe_unused]] bool is_master,
                       StopToken::pointer stop_token)
{
    for (int i = 0; i < coro_cnt; ++i)
    {
        mctx.yield_to_worker(i);
    }

    while (true)
    {
        uint64_t next_coro_id;

        // if (dsm->poll_rdma_cq_once(next_coro_id))
        // {
        //     mctx.yield_to_worker(next_coro_id);
        // }

        // NOTE: poll one each time
        // because we want to prioritized other operations
        dsm->try_master_coro_poll(&mctx, 1);

        if (c_.is_local)
        {
            auto &local_hot_wait_queue = page_cache().lock_pending_queue();
            if (!local_hot_wait_queue.empty())
            {
                next_coro_id = local_hot_wait_queue.front();
                local_hot_wait_queue.pop();
                mctx.yield_to_worker(next_coro_id);
            }
        }
        else
        {
            if (!hot_wait_queue.empty())
            {
                next_coro_id = hot_wait_queue.front();
                hot_wait_queue.pop();
                mctx.yield_to_worker(next_coro_id);
            }
        }

        // make these coros prioritized
        while (true)
        {
            auto hot_waiting_coro = mctx.next_hot_waiting_coro();
            if (hot_waiting_coro)
            {
                mctx.yield_to_worker(*hot_waiting_coro);
            }
            else
            {
                break;
            }
        }

        // // handle rpc
        // DSM::msg_desc_t msg_desc[16];
        // auto recv_nr = dsm->unreliable_try_recv_no_cpy(msg_desc, 16);
        // for (size_t i = 0; i < recv_nr; ++i)
        // {
        //     rpc::Response *resp = (rpc::Response *) msg_desc[i].msg_addr;
        //     CHECK_EQ(resp->hdr.type, rpc::RPCType::kAlloc);
        //     auto *alloc_resp = (rpc::AllocResponse *) resp;
        //     rpc::RpcContext *ctx =
        //         (rpc::RpcContext *) alloc_resp->hdr.rpc_context;
        //     *(uint64_t *) ctx->data = alloc_resp->addr;
        //     mctx.yield_to_worker(resp->hdr.from_coro_id);
        // }
        // dsm->return_buf_no_cpy(msg_desc, recv_nr);

        DCHECK_LE(coro_finished_nr, coro_cnt);
        if (unlikely(coro_finished_nr == (size_t) coro_cnt))
        {
            CHECK(stop_token->stop_requested());
            return;
        }
    }
}

// Local Locks
inline bool Tree::acquire_local_lock(GlobalAddress lock_addr,
                                     CoroContext *cxt,
                                     int coro_id)
{
    auto &node = local_locks[lock_addr.nodeID][lock_addr.offset / 8];

    uint64_t lock_val = node.ticket_lock.fetch_add(1);

    uint32_t ticket = lock_val << 32 >> 32;
    uint32_t current = lock_val >> 32;

    while (ticket != current)
    {  // lock failed

        if (cxt != nullptr)
        {
            hot_wait_queue.push(coro_id);
            cxt->yield_to_master();
        }

        current = node.ticket_lock.load(std::memory_order_relaxed) >> 32;
    }

    node.hand_time++;

    return node.hand_over;
}

inline bool Tree::can_hand_over(GlobalAddress lock_addr)
{
    auto &node = local_locks[lock_addr.nodeID][lock_addr.offset / 8];
    uint64_t lock_val = node.ticket_lock.load(std::memory_order_relaxed);

    uint32_t ticket = lock_val << 32 >> 32;
    uint32_t current = lock_val >> 32;

    if (ticket <= current + 1)
    {  // no pending locks
        node.hand_over = false;
    }
    else
    {
        node.hand_over = node.hand_time < define::kMaxHandOverTime;
    }
    if (!node.hand_over)
    {
        node.hand_time = 0;
    }

    return node.hand_over;
}

inline void Tree::releases_local_lock(GlobalAddress lock_addr)
{
    auto &node = local_locks[lock_addr.nodeID][lock_addr.offset / 8];

    node.ticket_lock.fetch_add((1ull << 32));
}

void Tree::index_cache_statistics()
{
    index_cache->statistics();
    index_cache->bench();
}

void Tree::clear_statistics()
{
    cache_hit_.fill(0);
    cache_miss_.fill(0);
}
GlobalAddress Tree::fast_alloc(size_t size, CoroContext *ctx)
{
    auto coro_id = ctx ? ctx->coro_id() : 0;
    return alloc_cache_[coro_id].alloc(size);
}

void Tree::fast_free(GlobalAddress gaddr, size_t size, CoroContext *ctx)
{
    auto coro_id = ctx ? ctx->coro_id() : 0;
    alloc_cache_[coro_id].free(gaddr, size);
}

GlobalAddress Tree::do_alloc(size_t size, CoroContext *ctx)
{
    std::ignore = ctx;
    auto *handle = get_avis_handle(ctx);
    if (handle)
    {
        auto ret = handle->alloc(size);
        CHECK(!ret.is_null());
        return ret;
    }
    else
    {
        auto ret = dsm->alloc_from(size, server_nid_);
        DCHECK_NE(ret.val, 0) << "** allocation get nullptr.";
        return ret;
    }
}
void Tree::prepare_alloc(CoroContext *ctx)
{
    auto coro_id = ctx ? ctx->coro_id() : 0;
    alloc_cache_[coro_id].prepare([this, ctx](size_t size) -> GlobalAddress
                                  { return do_alloc(size, ctx); });
}
void Tree::do_free(GlobalAddress gaddr, size_t size, CoroContext *ctx)
{
    if (likely(!gaddr.is_null()))
    {
        auto *handle = get_avis_handle(ctx);
        if (handle)
        {
            handle->free(gaddr, size);
        }
        else
        {
            dsm->free(gaddr, size);
        }
    }
    // LOG(INFO) << PRE(gaddr, size, ctx);
    std::ignore = gaddr;
    std::ignore = size;
    std::ignore = ctx;
}

avis::AvisHandle *Tree::get_avis_handle(CoroContext *ctx)
{
    if (c_.avis_)
    {
        if (ctx)
        {
            return avis_handle[ctx->coro_id()].get();
        }
        else
        {
            return master_avis_handle.get();
        }
    }
    return nullptr;
}

}  // namespace sherman