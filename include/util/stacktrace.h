#pragma once

#include <optional>

#include "util/gflags_dec.h"
#ifndef SHERMEM_STACKTRACE_H_
#define SHERMEM_STACKTRACE_H_

#define UNW_LOCAL_ONLY
#include <execinfo.h>
#include <fmt/format.h>
#include <glog/logging.h>

#include <iostream>

#include "libunwind.h"
#include "util/System.h"

namespace util
{
namespace detail
{
struct CachedInfo
{
    std::string function_name;
    size_t line;
    std::string file_name;
};
}  // namespace detail

struct Frame
{
    int fid;
    unw_word_t ip;
    unw_word_t sp;
    std::string name;
    unw_word_t offset;
    // function_name, fille_name, line
    mutable std::optional<std::tuple<std::string, std::string, std::string>>
        cache_info_;
    mutable std::optional<std::string> desc_;
    uint64_t addr() const
    {
        return ip + offset;
    }
    std::optional<std::string> desc() const
    {
        // NOTE: we need to distinguish
        // - We have not call sys::addr2desc yet
        // - We got that, but failed
        // So use empty string to denote failed call and
        // avoid calling again and again.
        if (!desc_)
        {
            auto try_desc = sys::addr2desc(ip);
            desc_ = try_desc.value_or("");
        }
        if (desc_ && !desc_->empty())
        {
            return *desc_;
        }
        return std::nullopt;
    }
    std::optional<std::string> line() const
    {
        fill_info();
        auto ln = std::get<2>(*cache_info_);
        if (ln.starts_with("?"))
        {
            return std::nullopt;
        }
        return ln;
    }
    std::string function_name() const
    {
        fill_info();
        auto fn = std::get<0>(*cache_info_);
        if (fn.starts_with("?"))
        {
            return name;
        }
        return fn;
    }
    std::string file_name() const
    {
        fill_info();
        auto fn = std::get<1>(*cache_info_);
        if (fn.starts_with("?"))
        {
            return FLAGS_binary;
        }
        return fn;
    }
    bool has_accurate_function_file() const
    {
        fill_info();
        bool acc_function = !std::get<0>(*cache_info_).starts_with("?");
        bool acc_file = !std::get<1>(*cache_info_).starts_with("?");
        return acc_function && acc_file;
    }
    void fill_info() const
    {
        if (!cache_info_)
        {
            cache_info_ = sys::addr2fn_file_ln(ip);
        }
    }
};
class StackTrace
{
public:
    StackTrace()
    {
        unw_cursor_t cursor;
        unw_context_t uc;
        unw_word_t ip, sp;
        char buf[4096];
        unw_word_t offset;
        unw_getcontext(&uc);           // store registers
        unw_init_local(&cursor, &uc);  // initialze with context

        int fid = 0;
        while (unw_step(&cursor) > 0)
        {  // unwind to older stack frame
            unw_get_reg(&cursor, UNW_REG_IP, &ip);  // read register, rip
            unw_get_reg(&cursor, UNW_REG_SP, &sp);  // read register, rbp
            unw_get_proc_name(
                &cursor, buf, 4095, &offset);  // get name and offset

            frames_.emplace_back(Frame{
                .fid = fid,
                .ip = ip,
                .sp = sp,
                .name = std::string(buf),
                .offset = offset,
            });

            fid++;
        }
    }

    const Frame &frame(size_t ith) const
    {
        return frames_[ith];
    }
    const auto &frames() const
    {
        return frames_;
    }
    size_t size() const
    {
        return frames_.size();
    }

private:
    std::vector<Frame> frames_;
};

inline std::ostream &operator<<(std::ostream &os, const Frame &frame)
{
    os << fmt::format(
        "    #{} {:#x} in {} ", frame.fid, frame.ip, frame.function_name());

    bool acc = frame.has_accurate_function_file();
    if (acc)
    {
        os << fmt::format("{}:{}", frame.file_name(), *frame.line());
    }
    else
    {
        os << fmt::format("({}+{:#x})", frame.file_name(), frame.ip);
    }
    return os;
}

inline std::ostream &operator<<(std::ostream &os, const StackTrace &st)
{
    auto flags = os.flags();

    os << std::endl;
    for (const auto &frame : st.frames())
    {
        os << frame << std::endl;
    }

    os.flags(flags);
    return os;
}

}  // namespace util

#endif