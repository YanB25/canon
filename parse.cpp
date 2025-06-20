#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <iostream>
#include <optional>
#include <string>

struct Item
{
    int64_t key_size;
    int64_t value_size;
    int64_t hashed_key;
    int64_t is_set;
};

inline std::ostream &operator<<(std::ostream &os, const Item &item)
{
    os << "{" << item.key_size << ", " << item.value_size << ", "
       << item.hashed_key << ", " << item.is_set << "}";
    return os;
}

std::string twitter_trace_fn(const std::string &cluster, const std::string &cid)
{
    return "cluster." + cluster + "/" + cid + ".bin";
}
struct Record
{
    size_t key_size;
    size_t value_size;
    bool is_set;
    std::string key;
};
inline std::ostream &operator<<(std::ostream &os, const Record &item)
{
    os << "{" << item.key_size << ", " << item.value_size << ", " << item.key
       << ", " << item.is_set << "}";
    return os;
}
struct TwitterParser
{
public:
    struct RecordLayout
    {
        uint32_t key_size;
        uint32_t value_size;
        uint8_t is_set;
        char key[];
    } __attribute__((packed));
    TwitterParser(const std::string &file) : file_(file)
    {
        fd_ = open(file.c_str(), O_RDONLY);
        if (fd_ < 0)
        {
            std::cerr << "Failed to open" << std::endl;
            exit(-1);
        }
        struct stat stat_buf;
        int rc = fstat(fd_, &stat_buf);
        size_t file_size = stat_buf.st_size;
        buf_ = (const Item *) mmap(
            nullptr, file_size, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (buf_ == MAP_FAILED)
        {
            std::cerr << "Failed to mmap" << std::endl;
            exit(-1);
        }

        remain_op_ = *(uint64_t *) buf_;
        cursor_ += sizeof(uint64_t);
    }
    auto remain_op()
    {
        return remain_op_;
    }
    std::optional<Record> next()
    {
        if (remain_op_ <= 0)
        {
            return {};
        }
        std::cout << cursor_ << std::endl;
        const auto &record = *(RecordLayout *) (((char *) buf_) + cursor_);
        size_t key_size = record.key_size;
        size_t value_size = record.value_size;
        bool is_set = record.is_set;
        // std::cout << "DEBUG: " << key_size << ", " << value_size
        //           << ", is_set: " << is_set << std::endl;
        std::string key = std::string(record.key, key_size);
        cursor_ += sizeof(RecordLayout) + key_size;
        // std::cout << "DEBUG: " << key_size << ", " << value_size << ", " <<
        // is_set
        //           << ", " << key << std::endl;
        remain_op_--;
        return Record{key_size, value_size, is_set, key};
    }

private:
    std::string file_{};
    int fd_{};
    const void *buf_{};
    uint64_t cursor_{};
    ssize_t remain_op_{};
};

int main()
{
    TwitterParser parser(
        "/home/yanbin/workspace/bin/twitter-trace/cluster50.sort-8");

    std::cout << parser.remain_op() << std::endl;
    for (size_t i = 0; i < 8; ++i)
    {
        std::cout << *parser.next() << std::endl;
    }
}
