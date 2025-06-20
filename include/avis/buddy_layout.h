#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "avis/config.h"
#include "glog/logging.h"
#include "util/Likely.h"

namespace avis
{
class PostorderTranversal
{
public:
    PostorderTranversal(size_t n) : n_(n)
    {
        idx_to_val_.resize(node_nr());
        val_to_idx_.resize(node_nr());
        int idx = 0;
        post_traversal(idx_to_val_, idx, 0);
        for (size_t i = 0; i < idx_to_val_.size(); ++i)
        {
            val_to_idx_[idx_to_val_[i]] = i;
        }
    }
    size_t page_nr() const
    {
        return n_;
    }
    size_t node_nr() const
    {
        return 2 * n_ - 1;
    }
    int to_val(int idx) const noexcept
    {
        return idx_to_val_[idx];
    }
    int to_idx(int val) const noexcept
    {
        return val_to_idx_[val];
    }

    const auto &idx_to_val() const noexcept
    {
        return idx_to_val_;
    }
    const auto &val_to_idx() const noexcept
    {
        return val_to_idx_;
    }

private:
    size_t n_;
    std::vector<int> idx_to_val_;
    std::vector<int> val_to_idx_;

    void post_traversal(std::vector<int> &arr, int &idx, int node_id)
    {
        if ((size_t) node_id >= node_nr())
        {
            return;
        }
        auto left_son = node_id * 2 + 1;
        if ((size_t) left_son < node_nr())
        {
            post_traversal(arr, idx, left_son);
        }
        auto right_son = node_id * 2 + 2;
        if ((size_t) right_son < node_nr())
        {
            post_traversal(arr, idx, right_son);
        }
        arr[idx] = node_id;
        idx++;
    }
};

inline const PostorderTranversal &getPostorderTraversal(size_t n)
{
    static std::unordered_map<size_t, PostorderTranversal> cache;
    static std::mutex mu_;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache.find(n);
    if (unlikely(it == cache.end()))
    {
        it = cache.emplace(n, n).first;
    }
    return it->second;
}

class Buddy
{
public:
    Buddy(size_t total_size, size_t page_size)
        : order_(getPostorderTraversal(total_size / page_size))
    {
        CHECK_EQ(total_size % page_size, 0);
    }
    std::optional<int> parent(int i) const
    {
        if (unlikely(i == 0))
        {
            return std::nullopt;
        }
        return (i - 1) / 2;
    }
    std::optional<int> left_son(int i) const
    {
        auto ret = 2 * i + 1;
        if (ret < node_nr())
        {
            return ret;
        }
        return std::nullopt;
    }
    std::optional<int> right_son(int i) const
    {
        auto ret = 2 * i + 2;
        if (ret < node_nr())
        {
            return ret;
        }
        return std::nullopt;
    }
    size_t page_nr() const
    {
        return order_.page_nr();
    }
    constexpr int depth() const
    {
        return log2(page_nr());
    }
    constexpr int depth(int i) const
    {
        return log2(i + 1);
    }
    constexpr int reverse_depth(int i) const
    {
        return depth() - depth(i);
    }
    constexpr int height(int i) const
    {
        return reverse_depth(i);
    }
    constexpr int node_nr() const
    {
        return 2 * page_nr() - 1;
    }
    constexpr size_t meta_bytes() const
    {
        auto ret = node_nr() / 8;
        ret += 8 - (ret % 8);
        return ret;
    }
    std::vector<int> get_ancestor_node_ids(int i) const
    {
        std::vector<int> ret;
        ret.reserve(depth());
        while (true)
        {
            auto p = parent(i);
            if (p)
            {
                ret.push_back(*p);
                i = *p;
            }
            else
            {
                break;
            }
        }
        return ret;
    }
    std::vector<int> get_descendant_node_ids(int i) const
    {
        std::vector<int> ret;
        ret.reserve(node_nr() / 2);
        do_get_descendants(ret, i);
        // don't include i
        ret.pop_back();
        return ret;
    }
    /**
     * get_descendant_position returns the (byte_offset, byte_nr) pair
     * of all the descendants of node_id.
     * @param i node_id
     * @param include_me whether or not the node_id should test
     * @return (byte_offset, byte_nr)
     */
    std::pair<int, size_t> get_descendant_bit_position(int i) const
    {
        auto leftest = leftest_son(i);
        auto leftest_pos_in_meta = node_id_to_bit_offset(leftest);
        auto h = height(i);
        auto bit_nr = subtree_node_nr(h);
        // "me" (the `i`) should not be included in descendants
        // -1 to exclude "me"
        return {leftest_pos_in_meta, bit_nr - 1};
    }
    size_t subtree_node_nr(int height) const
    {
        return (1ull << (height + 1)) - 1;
    }
    int leftest_son(int i) const
    {
        while (true)
        {
            auto left = left_son(i);
            if (left)
            {
                i = *left;
            }
            else
            {
                break;
            }
        }
        return i;
    }

    std::pair<int, int> node_ranges_in_depth(int depth) const
    {
        auto left = pow(2, depth) - 1;
        auto node_nr_in_depth = pow(2, depth);
        auto right = left + node_nr_in_depth;
        return {left, right};
    }

    size_t node_id_to_bit_offset(int id) const
    {
        if (avis::Config::ins().buddy_use_postorder())
        {
            return order_.to_idx(id);
        }
        else
        {
            return id;
        }
    }
    size_t bit_offset_to_node_id(size_t offset) const
    {
        if (avis::Config::ins().buddy_use_postorder())
        {
            return order_.to_val(offset);
        }
        else
        {
            return offset;
        }
    }

    std::unordered_set<int> ancestor_unit_set(int i, size_t unit_size) const
    {
        std::unordered_set<int> ret;
        auto ancestors = get_ancestor_node_ids(i);
        for (auto node_id : ancestors)
        {
            auto bit_offset = node_id_to_bit_offset(node_id);
            auto unit_id = bit_offset / 8 / unit_size;
            ret.insert(unit_id);
        }
        return ret;
    }

private:
    const PostorderTranversal &order_;

    void do_get_descendants(std::vector<int> &ret, int i) const
    {
        auto left = left_son(i);
        if (left)
        {
            do_get_descendants(ret, *left);
        }
        auto right = right_son(i);
        if (right)
        {
            do_get_descendants(ret, *right);
        }
        ret.push_back(i);
    }
};

}  // namespace avis