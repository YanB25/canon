#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "glog/logging.h"
#include "util/Rand.h"

namespace twitter
{
struct Record
{
    uint32_t key_size;
    uint32_t value_size;
    uint8_t is_set;
    const char *key;

    void fill_key(char *buf, size_t key_size)
    {
        memcpy(buf, key, key_size);
    }
    void fill_value(char *buf, size_t value_size)
    {
        fast_pseudo_fill_buf(buf, value_size);
    }
};
inline std::ostream &operator<<(std::ostream &os, const Record &item)
{
    os << "{" << item.key_size << ", " << item.value_size << ", "
       << std::string(item.key, item.key_size) << ", " << (bool) item.is_set
       << "}";
    return os;
}
struct Parser
{
public:
    struct RecordLayout
    {
        uint32_t key_size;
        uint32_t value_size;
        uint8_t is_set;
        char key[];
    } __attribute__((packed));

    constexpr static bool kDebug = true;

    Parser(const std::string &file) : file_(file)
    {
        fd_ = open(file.c_str(), O_RDONLY);
        LOG(INFO) << "[parser] opening " << file;
        if (fd_ < 0)
        {
            PLOG(FATAL) << "Failed to open " << file;
        }
        struct stat stat_buf;
        int rc = fstat(fd_, &stat_buf);
        PLOG_IF(FATAL, rc != 0) << "** Failed to fstat for fd = " << fd_;
        file_size_ = stat_buf.st_size;
        buf_ = mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (buf_ == MAP_FAILED)
        {
            PLOG(FATAL) << "Failed to mmap";
        }

        total_op_ = *(uint64_t *) buf_;
        remain_op_ = total_op_;
        cursor_ = sizeof(uint64_t);
    }
    size_t total_nr() const
    {
        return total_op_;
    }
    size_t remain_nr() const
    {
        return remain_op_;
    }
    ~Parser()
    {
        auto ret = munmap(buf_, file_size_);
        PLOG_IF(FATAL, ret < 0) << "** Failed to munmap";
        ret = close(fd_);
        PLOG_IF(FATAL, ret < 0) << "** Failed to close file";
        if constexpr (kDebug)
        {
            LOG(WARNING) << "DEBUG: parset has unique key " << uni_key_.size();
            static std::map<std::string, size_t> s;
            static std::mutex mu;
            std::lock_guard<std::mutex> lk(mu);

            s.insert(uni_key_.begin(), uni_key_.end());
            uint64_t sum_size = 0;
            for (const auto &[key, size] : s)
            {
                sum_size += size;
            }
            LOG(WARNING) << "Combined: " << s.size()
                         << ", total_object_size: " << sum_size << " with "
                         << s.size() << " unique keys";
        }
    }
    std::optional<Record> peak()
    {
        if (remain_op_ <= 0)
        {
            // LOG(WARNING) << "TODO: trace use up. wrap around: " << total_op_;
            // remain_op_ = *(uint64_t *) buf_;
            // cursor_ = sizeof(uint64_t);
            return {};
        }
        const auto &record = *(RecordLayout *) (((char *) buf_) + cursor_);
        return Record{
            record.key_size, record.value_size, record.is_set, record.key};
    }
    std::optional<Record> next()
    {
        auto ret = peak();
        if (!ret)
        {
            return {};
        }
        cursor_ += sizeof(RecordLayout) + ret->key_size;
        remain_op_--;
        if constexpr (kDebug)
        {
            uni_key_[std::string(ret->key, ret->key_size)] =
                ret->key_size + ret->value_size;
            if (ret->is_set)
            {
                debug_obj_size_ += ret->key_size + ret->value_size;
            }
        }
        return ret;
    }
    bool empty() const
    {
        return remain_op_ <= 0;
    }

private:
    std::string file_{};
    int fd_{};
    void *buf_{};
    size_t file_size_{};
    uint64_t cursor_{};
    ssize_t remain_op_{};
    ssize_t total_op_{};

    size_t debug_obj_size_{0};
    std::map<std::string, size_t> uni_key_;
};

}  // namespace twitter