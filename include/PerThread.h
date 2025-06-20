#pragma once
#ifndef PER_THREAD_H_
#define PER_THREAD_H_

#include <list>

#include "Common.h"
#include "util/Likely.h"
#include "util/UP.h"
#include "util/Util.h"
#include "util/lock/RWLock.h"

template <typename T, size_t kAligned = 64>
class Aligned
{
public:
    template <typename... Args>
    Aligned(Args &&...args) : obj_(std::forward<Args>(args)...)
    {
    }

    operator T &()
    {
        return obj_;
    }
    operator const T &() const
    {
        return obj_;
    }

    T &get()
    {
        return obj_;
    }
    const T &get() const
    {
        return obj_;
    }
    template <typename U, size_t S>
    friend std::ostream &operator<<(std::ostream &os, const Aligned<U, S> &a);

private:
    alignas(kAligned) T obj_;
};
static_assert(sizeof(Aligned<int>) % 64 == 0);

template <typename T, size_t kSize>
inline std::ostream &operator<<(std::ostream &os, const Aligned<T, kSize> &a)
{
    os << a.obj_;
    return os;
}

template <typename T, size_t kSize, size_t kAlignment = 64>
class AlignedArray
{
public:
    template <typename... Args>
    AlignedArray(Args &&...args) : arr_(std::forward<Args>(args)...)
    {
    }
    const T &operator[](size_t idx) const
    {
        return arr_[idx].get();
    }
    T &operator[](size_t idx)
    {
        return arr_[idx].get();
    }
    size_t size() const
    {
        return arr_.size();
    }

private:
    std::array<Aligned<T, kAlignment>, kSize> arr_{};
};

template <typename T, size_t kSize, size_t kAlignment = 64>
class AlignedHeapArray
{
public:
    using array_t = std::array<Aligned<T, kAlignment>, kSize>;
    AlignedHeapArray() : arr_(std::make_unique<array_t>())
    {
    }

    const T &operator[](size_t idx) const
    {
        return arr()[idx].get();
    }
    T &operator[](size_t idx)
    {
        return arr()[idx].get();
    }
    size_t size() const
    {
        return arr().size();
    }

private:
    array_t &arr()
    {
        return *arr_;
    }
    const array_t &arr() const
    {
        return *arr_;
    }
    std::unique_ptr<array_t> arr_;
};

template <typename T, size_t kAlignment = 64>
class AlignedVector
{
public:
    AlignedVector() noexcept = default;
    explicit AlignedVector(size_t n, const T &value) noexcept : vec_(n, value)
    {
    }
    explicit AlignedVector(size_t count) : vec_(count)
    {
    }
    template <typename InputIt>
    AlignedVector(InputIt first, InputIt last) : vec_(first, last)
    {
    }
    AlignedVector(const AlignedVector &) = default;
    AlignedVector &operator=(const AlignedVector &) = default;
    AlignedVector(AlignedVector &&) noexcept = default;
    AlignedVector &operator=(AlignedVector &&) noexcept = default;
    AlignedVector(std::initializer_list<T> init) : vec_(init)
    {
    }
    T &at(size_t pos)
    {
        return vec_[pos];
    }
    const T &at(size_t pos) const
    {
        return vec_[pos];
    }
    T *data()
    {
        return vec_.data();
    }
    const T *data() const
    {
        return vec_.data();
    }

    template <typename... Args>
    void emplace_back(Args &&...args)
    {
        vec_.emplace_back(std::forward<Args>(args)...);
    }
    template <typename T2>
    void push_back(T2 &&obj)
    {
        vec_.push_back(std::forward<T2>(obj));
    }
    T &operator[](size_t pos)
    {
        return vec_[pos].get();
    }
    const T &operator[](size_t pos) const
    {
        return vec_[pos].get();
    }
    T &front()
    {
        return vec_.front();
    }
    const T &front() const
    {
        return vec_.front();
    }
    T &back()
    {
        return vec_.back();
    }
    const T &back() const
    {
        return vec_.back();
    }
    bool empty() const
    {
        return vec_.empty();
    }
    size_t size() const
    {
        return vec_.size();
    }
    size_t max_size() const
    {
        return vec_.max_size();
    }
    size_t capacity() const
    {
        return vec_.capacity();
    }
    void reserve(size_t new_size)
    {
        vec_.reserve(new_size);
    }
    void clear()
    {
        vec_.clear();
    }
    void pop_back()
    {
        return vec_.pop_back();
    }
    auto begin()
    {
        return vec_.begin();
    }
    auto cbegin() const
    {
        return vec_.cbegin();
    }
    auto end()
    {
        return vec_.end();
    }
    auto cend() const
    {
        return vec_.cend();
    }
    auto rbegin()
    {
        return vec_.rbegin();
    }
    auto rend()
    {
        return vec_.rend();
    }
    auto crbegin()
    {
        return vec_.crbegin();
    }
    auto crend()
    {
        return vec_.crend();
    }

private:
    std::vector<Aligned<T>> vec_;
};

template <typename T>
class Perthread
{
public:
    constexpr static size_t kCachelineSize = 64;
    using value_type = T;
    using size_type = std::size_t;
    using pointer = value_type *;
    using const_pointer = const value_type *;

    using aligned_t = Aligned<T, kCachelineSize>;
    using array_t = std::array<aligned_t, kCorePerNuma>;

    Perthread()
    {
        per_thread_ = std::make_unique<array_t>();
    }

    value_type &operator[](size_t idx)
    {
        return arr()[idx].get();
    }
    const value_type &operator[](size_t idx) const
    {
        return arr()[idx].get();
    }
    size_type size() const
    {
        return arr().size();
    }
    bool empty() const
    {
        return arr().empty();
    }
    auto begin()
    {
        return arr().begin();
    }
    auto end()
    {
        return arr().end();
    }
    constexpr auto begin() const
    {
        return arr().begin();
    }
    constexpr auto end() const
    {
        return arr().end();
    }
    auto cbegin() const
    {
        return arr().cbegin();
    }
    auto cend() const
    {
        return arr().cend();
    }

    value_type &current()
    {
        auto tid = util::get_thread_id();
        DCHECK_LT(tid, kCorePerNuma);
        return arr()[tid];
    }
    const value_type &current() const
    {
        auto tid = util::get_thread_id();
        DCHECK_LT(tid, kCorePerNuma);
        return arr()[tid];
    }

    template <typename U>
    void fill(const U &val)
    {
        for (auto &item : arr())
        {
            item.get() = val;
        }
    }

    template <typename AccF, typename U>
    U accumulate(const AccF &acc, U init = U{}) const
    {
        U ret = init;
        for (const auto &item : arr())
        {
            const T &i = item.get();
            ret = acc(ret, i);
        }
        return ret;
    }

    template <typename PF>
    size_t count_if(const PF &p) const
    {
        size_t ret = 0;
        for (const auto &item : arr())
        {
            ret += p(item);
        }
        return ret;
    }

    T sum() const
    {
        T ret{};
        for (const auto &item : arr())
        {
            ret += item.get();
        }
        return ret;
    }

    template <typename U>
    friend std::ostream &operator<<(std::ostream &os, const Perthread<U> &p);

private:
    array_t &arr()
    {
        return *per_thread_;
    }
    const array_t &arr() const
    {
        return *per_thread_;
    }
    std::unique_ptr<array_t> per_thread_;
};
template <typename T>
inline std::ostream &operator<<(std::ostream &os, const Perthread<T> &p)
{
    os << util::pre(p.arr());
    return os;
}

class CoroContext;

template <typename T>
class PerCoro
{
public:
    T &current(int cid);
    const T &current(int cid) const;
    T &current(const CoroContext *);
    const T &current(const CoroContext *) const;

private:
    Perthread<std::array<T, define::kMaxCoroNr>> inner_;
};

#endif