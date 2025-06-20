#pragma once
#include <iostream>

#include "util/Pre.h"

struct RemoteMetrics
{
    size_t read_nr_{};
    size_t read_size_{};
    size_t write_nr_{};
    size_t write_size_{};
    size_t faa_nr_{};
    size_t faa_size_{};
    size_t cas_nr_{};
    size_t cas_size_{};
    size_t cas_ok_nr_{};

    RemoteMetrics &operator+=(const RemoteMetrics &rhs)
    {
        read_nr_ += rhs.read_nr_;
        read_size_ += rhs.read_size_;
        write_nr_ += rhs.write_nr_;
        write_size_ += rhs.write_size_;
        faa_nr_ += rhs.faa_nr_;
        faa_size_ += rhs.faa_size_;
        cas_nr_ += rhs.cas_nr_;
        cas_size_ += rhs.cas_size_;
        cas_ok_nr_ += rhs.cas_ok_nr_;
        return *this;
    }
    RemoteMetrics operator+(const RemoteMetrics &rhs) const
    {
        RemoteMetrics ret = *this;
        ret += rhs;
        return ret;
    }
    void read(size_t size)
    {
        read_nr_++;
        read_size_ += size;
    }
    void write(size_t size)
    {
        write_nr_++;
        write_size_ += size;
    }
    void faa(size_t size)
    {
        faa_nr_++;
        faa_size_ += size;
    }
    void cas(size_t size, bool ok)
    {
        cas_nr_++;
        if (ok)
        {
            cas_ok_nr_++;
        }
        cas_size_ += size;
    }
    size_t io_nr() const
    {
        return read_nr_ + write_nr_ + faa_nr_ + cas_nr_;
    }
    size_t io_size() const
    {
        return read_size_ + write_size_ + faa_size_ + cas_size_;
    }
    void reset()
    {
        *this = RemoteMetrics{};
    }
};

inline std::ostream &operator<<(std::ostream &os, const RemoteMetrics &m)
{
    os << "{read: " << m.read_nr_ << " (" << util::pre_byte(m.read_size_)
       << "), write: " << m.write_nr_ << " (" << util::pre_byte(m.write_size_)
       << "), faa: " << m.faa_nr_ << " (" << util::pre_byte(m.faa_size_)
       << "), cas: " << m.cas_nr_ << " (" << util::pre_byte(m.cas_size_)
       << "), total: " << m.io_nr() << " (" << util::pre_byte(m.io_size())
       << ")}";
    return os;
}

struct AllocMetrics
{
    uint64_t allocated_bytes_{};
    uint64_t deallocated_bytes_{};
    uint64_t allocated_nr_{};
    uint64_t deallocated_nr_{};
    AllocMetrics &operator+=(const AllocMetrics &rhs)
    {
        allocated_bytes_ += rhs.allocated_bytes_;
        deallocated_bytes_ += rhs.deallocated_bytes_;
        allocated_nr_ += rhs.allocated_nr_;
        deallocated_nr_ += rhs.deallocated_nr_;
        return *this;
    }
    void record_alloc(size_t size)
    {
        if (size)
        {
            allocated_bytes_ += size;
            allocated_nr_++;
        }
    }
    void record_dealloc(size_t size)
    {
        if (size)
        {
            deallocated_bytes_ += size;
            deallocated_nr_++;
        }
    }
    void reset()
    {
        allocated_bytes_ = 0;
        allocated_nr_ = 0;
        deallocated_bytes_ = 0;
        deallocated_nr_ = 0;
    }
    size_t ongoing_bytes() const
    {
        return allocated_bytes_ - deallocated_bytes_;
    }
    size_t ongoing_nr() const
    {
        return allocated_nr_ - deallocated_nr_;
    }
};

inline AllocMetrics operator+(const AllocMetrics &lhs, const AllocMetrics &rhs)
{
    AllocMetrics ret = lhs;
    ret += rhs;
    return ret;
}

inline std::ostream &operator<<(std::ostream &os, const AllocMetrics &u)
{
    os << "{allocated: " << u.allocated_bytes_ << " ("
       << util::pre_byte(u.allocated_bytes_) << ")"
       << ", nr: " << util::pre_num(u.allocated_nr_)
       << ", deallocated: " << u.deallocated_bytes_ << " ("
       << util::pre_byte(u.deallocated_bytes_) << ")"
       << ", nr: " << util::pre_num(u.deallocated_nr_)
       << ", ongoing: " << util::pre_byte(u.ongoing_bytes())
       << " nr: " << util::pre_num(u.ongoing_nr()) << "}";
    return os;
}

struct OpMetric
{
    size_t put_ok{};
    size_t put_nr{};
    size_t del_nr{};
    size_t del_hit{};
    size_t del_miss{};
    size_t get_nr{};
    size_t get_hit{};
    size_t get_miss{};

    void reset()
    {
        put_ok = 0;
        put_nr = 0;
        del_nr = 0;
        del_hit = 0;
        del_miss = 0;
        get_nr = 0;
        get_hit = 0;
        get_miss = 0;
    }
    OpMetric &operator+=(const OpMetric &rhs)
    {
        put_ok += rhs.put_ok;
        put_nr += rhs.put_nr;
        del_nr += rhs.del_nr;
        del_hit += rhs.del_hit;
        del_miss += rhs.del_miss;
        get_nr += rhs.get_nr;
        get_hit += rhs.get_hit;
        get_miss += rhs.get_miss;
        return *this;
    }
};

inline OpMetric operator+(const OpMetric &lhs, const OpMetric &rhs)
{
    OpMetric m = lhs;
    m += rhs;
    return m;
}

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::OpMetric &v)
{
    os << "{OpMetric ";
    os << "put_ok: " << util::pre(v.put_ok);
    os << ", put_nr: " << util::pre(v.put_nr);
    os << ", del_nr: " << util::pre(v.del_nr);
    os << ", del_hit: " << util::pre(v.del_hit);
    os << ", del_miss: " << util::pre(v.del_miss);
    os << ", get_nr: " << util::pre(v.get_nr);
    os << ", get_hit: " << util::pre(v.get_hit);
    os << ", get_miss: " << util::pre(v.get_miss);
    os << "}";
    return os;
}

struct CacheMetric
{
    size_t write_nr_{};
    size_t read_nr_{};
    size_t write_hit_{};
    size_t read_hit_{};
    void record_write_hit()
    {
        write_nr_++;
        write_hit_++;
    }
    void record_write_miss()
    {
        write_nr_++;
    }
    void record_read_hit()
    {
        read_nr_++;
        read_hit_++;
    }
    void record_read_miss()
    {
        read_nr_++;
    }
    double write_hit_rate() const
    {
        return 1.0 * write_hit_ / write_nr_;
    }
    double write_miss_rate() const
    {
        return 1 - write_hit_rate();
    }
    double read_hit_rate() const
    {
        return 1.0 * read_hit_ / read_nr_;
    }
    double read_miss_rate() const
    {
        return 1 - read_hit_rate();
    }
    size_t op_nr() const
    {
        return write_nr_ + read_nr_;
    }
    size_t hit_nr() const
    {
        return write_hit_ + read_hit_;
    }
    size_t miss_nr() const
    {
        return op_nr() - hit_nr();
    }
    double hit_rate() const
    {
        return 1.0 * hit_nr() / op_nr();
    }
};

inline std::ostream &operator<<(std::ostream &os,
                                [[maybe_unused]] const ::CacheMetric &v)
{
    os << "{CacheMetric ";
    os << "op_hit: " << v.hit_rate() << ", ";
    os << "write_hit: " << v.write_hit_rate() << ", ";
    os << "read_hit: " << v.read_hit_rate() << ", ";
    os << "op: " << v.op_nr();
    os << "}";
    return os;
}