#pragma once

#include <fmt/format.h>

#include <array>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <optional>
#include <string>

#include "glog/logging.h"
#include "util/gflags_dec.h"

namespace sys
{
class Process
{
public:
    Process(const std::string &p)
    {
        std::array<char, 128> buffer;

        FILE *pipe = popen(p.c_str(), "r");  // Replace "ls" with your command
        if (!pipe)
        {
            return;
        }

        std::string output;
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr)
        {
            output += buffer.data();
        }

        int status = pclose(pipe);
        PLOG_IF(FATAL, status != 0) << "** failed to pclose for " << p;

        output_.emplace(std::move(output));
    }

    const std::optional<std::string> &output() const
    {
        return output_;
    }
    std::optional<std::string> &output()
    {
        return output_;
    }

private:
    std::optional<std::string> output_;
};

inline std::optional<std::string> addr2line(uint64_t addr)
{
    auto ret = Process(fmt::format("addr2line {:#x} -e {}", addr, FLAGS_binary))
                   .output();
    // the last char is '\n'. Strip it.
    if (ret)
    {
        ret->resize(ret->size() - 1);
        return std::move(*ret);
    }
    return ret;
}

inline std::optional<std::pair<std::string, std::string>> addr2fnline(
    uint64_t addr)
{
    auto output =
        Process(fmt::format("addr2line {:#x} -e {} -f -C", addr, FLAGS_binary))
            .output();
    if (!output)
    {
        return std::nullopt;
    }
    if (output->starts_with("??"))
    {
        return std::nullopt;
    }

    std::string str = std::move(*output);

    std::vector<std::string> results;
    boost::split(results, str, boost::is_any_of("\n"));
    return std::make_pair(results[0], results[1]);
}

inline std::tuple<std::string, std::string, std::string> addr2fn_file_ln(
    uint64_t addr)
{
    auto output =
        Process(fmt::format("addr2line {:#x} -e {} -f -C", addr, FLAGS_binary))
            .output();
    if (!output)
    {
        return {"??", "??", "?"};
    }

    std::string str = std::move(*output);
    // auto pos = str.find_first_of('\n');
    // // do not include '\n'
    // auto fn = str.substr(0, pos);
    // // do not include the first and last '\n'
    // auto line = str.substr(pos + 1, str.size() - 2);
    // return std::make_pair(fn, line);

    std::vector<std::string> results;
    boost::split(results, str, boost::is_any_of("\n:"));
    return {results[0], results[1], results[2]};
}

inline std::optional<std::string> addr2desc(uint64_t addr)
{
    auto output =
        Process(fmt::format(
                    "addr2line {:#x} -e {} -f -C -p -i -r", addr, FLAGS_binary))
            .output();
    if (output)
    {
        std::string ret = std::move(*output);
        ret.resize(ret.size() - 1);
        return ret;
    }
    return std::nullopt;
}

}  // namespace sys