#pragma once

#include <atomic>
#include <cinttypes>
#include <tuple>
#include <utility>

#include "glog/logging.h"
#include "util/ASM.h"

namespace util
{
class TicketRWLock
{
public:
    TicketRWLock() : reader_writer_(0)
    {
        rw_ = (std::atomic<uint64_t> *) &reader_writer_;
        writer_ = (std::atomic<uint32_t> *) &reader_writer_;
        reader_ = (std::atomic<uint32_t> *) ((char *) &reader_writer_ + 4);
        writer_cur_ = (std::atomic<uint16_t> *) &reader_writer_;
        writer_tik_ = (std::atomic<uint16_t> *) ((char *) &reader_writer_ + 2);
        reader_cur_ = (std::atomic<uint16_t> *) ((char *) &reader_writer_ + 4);
        reader_tik_ = (std::atomic<uint16_t> *) ((char *) &reader_writer_ + 6);
        LOG(FATAL) << "** can not use ticket-based rw lock. Who goes first "
                      "with ticket? reader or writer?";
    }
    uint16_t fetch_writer_ticket()
    {
        return writer_tik_->fetch_add(1, std::memory_order_acquire);
    }
    uint16_t fetch_reader_ticket()
    {
        return reader_tik_->fetch_add(1, std::memory_order_acquire);
    }
    __attribute__((always_inline)) uint64_t load_val()
    {
        return rw_->load(std::memory_order_relaxed);
    }
    __attribute__((always_inline)) static std::
        tuple<uint16_t, uint16_t, uint16_t, uint16_t>
        destruct(uint64_t states)
    {
        uint16_t reader_tic = (states << 0) >> 48;
        uint16_t reader_cur = (states << 16) >> 48;
        uint16_t writer_tik = (states << 32) >> 48;
        uint16_t writer_cur = (states << 48) >> 48;
        return {reader_tic, reader_cur, writer_tik, writer_cur};
    }

    void write_lock()
    {
        // auto my_wtik = fetch_writer_ticket();
        // while (true)
        // {
        //     uint64_t val = load_val();
        //     auto [rtik, rcur, wtik, wcur] = destruct(val);
        //     if (rtik != rcur || wcur != wtik)
        //     {
        //         util::asms::cpu_relax();
        //         continue;
        //     }
        // }
    }

    // private:
    // MSB [reader_tic, reader_cur, writer_tic, writer_cur] LSB
    uint64_t reader_writer_{0};
    std::atomic<uint64_t> *rw_;
    std::atomic<uint32_t> *reader_;
    std::atomic<uint32_t> *writer_;
    std::atomic<uint16_t> *reader_tik_;
    std::atomic<uint16_t> *reader_cur_;
    std::atomic<uint16_t> *writer_tik_;
    std::atomic<uint16_t> *writer_cur_;
};
}  // namespace util