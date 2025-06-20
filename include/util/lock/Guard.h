#pragma once
#include "glog/logging.h"

namespace util
{
template <typename Lock>
class UniqueGuard
{
public:
    UniqueGuard(Lock &lock) : lock_(&lock)
    {
        DCHECK_NOTNULL(lock_)->write_lock();
    }
    // can not copy and move assign
    UniqueGuard(const UniqueGuard &) = delete;
    UniqueGuard &operator=(const UniqueGuard &) = delete;
    UniqueGuard &operator=(UniqueGuard &&rhs) = delete;
    // only move constructable
    UniqueGuard(UniqueGuard &&rhs)
    {
        lock_ = rhs.lock_;
        rhs.lock_ = nullptr;
    }
    ~UniqueGuard()
    {
        if (lock_)
        {
            lock_->write_unlock();
        }
    }

private:
    Lock *lock_;
};

template <typename Lock>
class SharedGuard
{
public:
    SharedGuard(Lock &lock) : lock_(&lock)
    {
        DCHECK_NOTNULL(lock_)->read_lock();
    }
    SharedGuard(const SharedGuard &) = delete;
    SharedGuard &operator=(const SharedGuard &) = delete;
    SharedGuard &operator=(SharedGuard &&) = delete;
    SharedGuard(SharedGuard &&rhs)
    {
        lock_ = rhs.lock_;
        rhs.lock_ = nullptr;
    }
    ~SharedGuard()
    {
        if (lock_)
        {
            lock_->read_unlock();
        }
    }

private:
    Lock *lock_;
};
}  // namespace util