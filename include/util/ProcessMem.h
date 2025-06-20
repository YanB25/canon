#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "glog/logging.h"
#include "util/Pre.h"

namespace util
{
struct MemInfoDesc
{
    void *begin_addr;
    void *end_addr;
    size_t addr_size;
    std::string perms;
    std::string offset;
    std::string dev;
    std::string inode;
    std::string pathname;
    bool is_anonymous() const
    {
        return pathname.empty();
    }
    bool locate_addr(const void *addr) const
    {
        return begin_addr <= addr && addr <= end_addr;
    }
};

inline std::ostream &operator<<(std::ostream &os, const MemInfoDesc &d)
{
    os << "{" << util::pre(d.begin_addr) << "-" << util::pre(d.end_addr) << " ("
       << util::pre_byte(d.addr_size) << ") " << d.perms << " " << d.offset
       << " " << d.dev << " " << d.inode << " " << d.pathname << "}";
    return os;
}

class KPageFlags
{
public:
    struct Flag
    {
        uint64_t LOCKED : 1;
        uint64_t ERROR : 1;
        uint64_t REFERENCED : 1;
        uint64_t UPTODATE : 1;
        uint64_t DIRTY : 1;
        uint64_t LRU : 1;
        uint64_t ACTIVE : 1;
        uint64_t SLAB : 1;
        uint64_t WRITEBACK : 1;
        uint64_t RECLAIM : 1;
        uint64_t BUDDY : 1;
        uint64_t MMAP : 1;
        uint64_t ANON : 1;
        uint64_t SWAPCACHE : 1;
        uint64_t SWAPBACKED : 1;
        uint64_t COMPOUND_HEAD : 1;
        uint64_t COMPOUND_TAIL : 1;
        uint64_t HUGE : 1;
        uint64_t UNEVICTABLE : 1;
        uint64_t HWPOISON : 1;
        uint64_t NOPAGE : 1;
        uint64_t KSM : 1;
        uint64_t THP : 1;
        uint64_t BALLOON : 1;
        uint64_t ZERO_PAGE : 1;
        uint64_t IDLE : 1;
    };
    KPageFlags()
    {
        auto pageflags = std::filesystem::path("/proc") / "kpageflags";

        fd_ = open(pageflags.c_str(), O_RDONLY);
        LOG_IF(FATAL, fd_ <= 0) << "** failed to open " << pageflags;
    }

    Flag flag(uint64_t pfn) const
    {
        Flag flags{};
        auto offset = pfn * sizeof(flags);
        int ret = pread(fd_, &flags, sizeof(flags), offset);
        PLOG_IF(FATAL, ret != sizeof(flags))
            << "** failed to pread at " << (void *) offset;
        return flags;
    }

    ~KPageFlags()
    {
        int ret = close(fd_);
        PLOG_IF(FATAL, ret) << "** failed to close file";
    }

private:
    int fd_;
};

// Memory physical virtual mapping
class MemoryPVMapping
{
public:
    struct PagemapEntry
    {
        uint64_t pfn : 55;            // 0-54
        unsigned int soft_dirty : 1;  // 55
        unsigned int exclusive : 1;   // 56
        unsigned int reserved : 4;    // 57-60
        unsigned int file_page : 1;   // 61
        unsigned int swapped : 1;     // 62
        unsigned int present : 1;     // 63
        constexpr bool is_swapped() const
        {
            return swapped;
        }
        constexpr bool is_present() const
        {
            return present;
        }
        constexpr bool is_file_page() const
        {
            return file_page;
        }
        constexpr bool is_shared_anon() const
        {
            return !is_file_page();
        }
        constexpr bool is_exclusively_mapped() const
        {
            return exclusive;
        }
        constexpr bool is_soft_dirty() const
        {
            return soft_dirty;
        }
        // ONLY IF is_present()
        constexpr uint64_t page_frame() const
        {
            return pfn;
        }
        // ONLY IF is_swapped()
        constexpr unsigned int swap_type() const
        {
            return pfn & 0xf;  // 0-4
        }
        // ONLY IF is_swapped()
        uint64_t swap_offset() const
        {
            return pfn >> 5;  // 5-54
        }
    } __attribute__((packed));
    static_assert(sizeof(PagemapEntry) == sizeof(uint64_t));

    constexpr static size_t kPageSize = 0x1000;
    MemoryPVMapping() : pid_(getpid())
    {
        auto pagemap =
            std::filesystem::path("/proc") / std::to_string(pid_) / "pagemap";

        pagemap_fd_ = open(pagemap.c_str(), O_RDONLY);
        LOG_IF(FATAL, pagemap_fd_ <= 0) << "** failed to open " << pagemap;
        CHECK_EQ(kPageSize, getpagesize())
            << "** Please change kPageSize to " << getpagesize();
    }
    [[nodiscard]] PagemapEntry describe(void *vaddr)
    {
        auto file_offset = (uint64_t) vaddr / kPageSize * sizeof(PagemapEntry);

        PagemapEntry entry;
        int ret = pread(pagemap_fd_, &entry, sizeof(entry), file_offset);
        PLOG_IF(FATAL, ret != sizeof(entry))
            << "** failed to pread at " << (void *) file_offset;

        return entry;
    }
    void *virt_to_phys(void *vaddr)
    {
        auto entry = describe(vaddr);
        return (void *) (entry.pfn * kPageSize +
                         ((uint64_t) vaddr % kPageSize));
    }
    ~MemoryPVMapping()
    {
        int ret = close(pagemap_fd_);
        PLOG_IF(FATAL, ret) << "** failed to close file";
    }

private:
    pid_t pid_;
    int pagemap_fd_;
};

inline std::ostream &operator<<(std::ostream &os,
                                MemoryPVMapping::PagemapEntry e)
{
    std::vector<const char *> flags;
    flags.reserve(4);
    if (e.is_present())
    {
        flags.push_back("present");
    }
    if (e.is_file_page())
    {
        flags.push_back("file_page");
    }
    else
    {
        flags.push_back("shared_anon");
    }
    if (e.is_exclusively_mapped())
    {
        flags.push_back("exclusive");
    }
    if (e.is_soft_dirty())
    {
        flags.push_back("soft_dirty");
    }

    if (e.is_swapped())
    {
        os << "{swap_offset: " << (void *) e.swap_offset()
           << ", swap_type: " << e.swap_type();
    }
    else
    {
        os << "{pfn: " << (void *) e.pfn;
    }
    if (!flags.empty())
    {
        pre_ctx ctx;
        ctx.quote_string = false;
        os << " " << util::pre(flags, ctx);
    }
    os << "}";

    return os;
}

class MemInfo
{
public:
    MemInfo() : MemInfo(getpid())
    {
    }
    MemInfo(pid_t pid) : pid_(pid)
    {
        auto filename =
            std::filesystem::path("/proc") / std::to_string(pid) / "maps";

        std::ifstream input(filename);
        for (std::string line; getline(input, line);)
        {
            std::vector<std::string> strs;
            // split by "\t" and " "
            boost::split(strs, line, boost::is_any_of("\t "));
            std::vector<std::string> not_empty_strs;
            // filter out any empty strings
            std::copy_if(strs.begin(),
                         strs.end(),
                         std::back_inserter(not_empty_strs),
                         [](const std::string &str) { return !str.empty(); });
            // break the addresses into two void *
            std::vector<std::string> addresses;
            boost::split(addresses, not_empty_strs[0], boost::is_any_of("-"));
            uint64_t begin_addr;
            uint64_t end_addr;
            sscanf(addresses[0].c_str(), "%lx", &begin_addr);
            sscanf(addresses[1].c_str(), "%lx", &end_addr);
            MemInfoDesc d{
                .begin_addr = (void *) begin_addr,
                .end_addr = (void *) end_addr,
                .addr_size = end_addr - begin_addr,
                .perms = not_empty_strs.size() > 1 ? not_empty_strs[1] : "",
                .offset = not_empty_strs.size() > 2 ? not_empty_strs[2] : "",
                .dev = not_empty_strs.size() > 3 ? not_empty_strs[3] : "",
                .inode = not_empty_strs.size() > 4 ? not_empty_strs[4] : "",
                .pathname = not_empty_strs.size() > 5 ? not_empty_strs[5] : ""};
            descs_.emplace_back(std::move(d));
        }
    }
    const std::vector<MemInfoDesc> &desc() const
    {
        return descs_;
    }
    pid_t pid() const
    {
        return pid_;
    }
    std::vector<MemInfoDesc> heaps() const
    {
        return filter([](const auto &t)
                      { return t.pathname.find("heap") != std::string::npos; });
    }
    std::vector<MemInfoDesc> stacks() const
    {
        return filter(
            [](const auto &t)
            { return t.pathname.find("stack") != std::string::npos; });
    }
    auto larger_than(uint64_t byte) const
    {
        return filter([byte](const auto &t) { return t.addr_size >= byte; });
    }
    auto anonymous() const
    {
        return filter([](const auto &t) { return t.is_anonymous(); });
    }
    using P = std::function<bool(const MemInfoDesc &)>;
    std::vector<MemInfoDesc> filter(const P &p) const
    {
        return util::filter(descs_, p);
    }

    template <typename T>
    std::optional<MemInfoDesc> locate(const T &obj) const
    {
        auto ret = filter([addr = (void *) &obj](const auto &t)
                          { return t.locate_addr(addr); });
        if (ret.empty())
        {
            return std::nullopt;
        }
        CHECK_EQ(ret.size(), 1);
        return ret.front();
    }

    void dump()
    {
        for (const auto &d : descs_)
        {
            LOG(INFO) << d;
        }
    }

private:
    pid_t pid_;
    std::vector<MemInfoDesc> descs_;
};
}  // namespace util