#pragma once
#ifndef PERTRONUS_RACEHASHING_KV_BLOCK_H_
#define PERTRONUS_RACEHASHING_KV_BLOCK_H_

#include <cstdint>
#include <cstring>

#include "./utils.h"
#include "GlobalAddress.h"

namespace patronus::hash
{

struct KVBlock
{
    // TODO: add checksum to KVBlock
    // Paper sec 3.3, there will be a corner case where one client is reading
    // @key and @value from the KVBlock. Meanwhile, the KVBlock is freed,
    // re-allocated, and be filled with the same @key but different @value.
    // TO detect this inconsistency, add checksum to the KVBlock.

    // format
    // | key_len | value_len | hash | key | value
    uint32_t key_len;
    uint32_t value_len;
    uint64_t hash;
    char buf[0];
} __attribute__((packed));

inline std::ostream &operator<<(std::ostream &os, const KVBlock &b)
{
    os << "{KVBlock key_len: " << b.key_len << ", value_len: " << b.value_len
       << ", hash: " << b.hash << ", buf: " << b.buf << "}";
    return os;
}

struct KVBlockView
{
    KVBlockView(void *buf, size_t key_len, size_t value_len)
        : buf_((char *) buf), key_len_(key_len), value_len_(value_len)
    {
    }
    size_t total_size() const
    {
        return sizeof(KVBlock) + key_len_ + value_len_;
    }
    uint32_t *key_len_buf()
    {
        return (uint32_t *) (buf_ + offsetof(KVBlock, key_len));
    }
    uint32_t *value_len_buf()
    {
        return (uint32_t *) (buf_ + offsetof(KVBlock, value_len));
    }
    uint64_t *hash_buf()
    {
        return (uint64_t *) (buf_ + offsetof(KVBlock, hash));
    }
    char *buffer_ptr()
    {
        return buf_ + offsetof(KVBlock, buf);
    }
    const char *buffer_ptr() const
    {
        return buf_ + offsetof(KVBlock, buf);
    }
    char *buffer() const
    {
        return buf_;
    }
    const char *kv_buffer_ptr() const
    {
        return buf_ + offsetof(KVBlock, buf);
    }
    BufferView key()
    {
        return {key_buf(), key_len_};
    }
    BufferView value()
    {
        return {value_buf(), value_len_};
    }
    void *key_buf()
    {
        return buffer_ptr();
    }
    const void *key_buf() const
    {
        return kv_buffer_ptr();
    }
    size_t key_len() const
    {
        return key_len_;
    }
    size_t value_len() const
    {
        return value_len_;
    }
    void *value_buf()
    {
        return buffer_ptr() + key_len_;
    }
    BufferView key_view()
    {
        return BufferView(key_buf(), key_len_);
    }
    BufferView value_view()
    {
        return BufferView(value_buf(), value_len_);
    }
    void fill_key(std::string_view key)
    {
        auto size = std::min(key_len_, key.size());
        memcpy(key_buf(), key.data(), size);
    }
    void fill_value(std::string_view value)
    {
        auto size = std::min(value_len_, value.size());
        memcpy(value_buf(), value.data(), size);
    }
    void fill_key_value(std::string_view key, std::string_view value)
    {
        fill_key(key);
        fill_value(value);
    }
    constexpr static size_t kvblock_fit_value(size_t kvblock_size,
                                              size_t key_len)
    {
        auto [actual_kvblock_size, _] =
            get_actual_kvblock_tagged_size(kvblock_size);
        return actual_kvblock_size - sizeof(KVBlock) - key_len;
    }

    char *buf_;
    size_t key_len_;
    size_t value_len_;
};

class KVBlockHandle
{
public:
    KVBlockHandle(GlobalAddress addr, KVBlock *buffer)
        : addr_(addr), buffer_(buffer)
    {
    }
    GlobalAddress remote_addr() const
    {
        return addr_;
    }
    KVBlock *buffer_addr() const
    {
        return buffer_;
    }
    size_t key_len() const
    {
        return buffer_->key_len;
    }
    size_t value_len() const
    {
        return buffer_->value_len;
    }
    void *kv_buf() const
    {
        return buffer_->buf;
    }
    size_t total_size() const
    {
        auto ret = KVBlockView{nullptr, key_len(), value_len()}.total_size();
        auto [actual, _] = get_actual_kvblock_tagged_size(ret);
        return actual;
    }
    auto operator<=>(const KVBlockHandle &rhs) const
    {
        return addr_ <=> rhs.addr_;
    }
    bool operator==(const KVBlockHandle &rhs) const
    {
        return addr_ == rhs.addr_;
    }

private:
    GlobalAddress addr_{};
    KVBlock *buffer_{nullptr};
};
inline std::ostream &operator<<(std::ostream &os, const KVBlockHandle &kbh)
{
    os << "{KVBlockHandle addr: " << kbh.remote_addr()
       << ", buffer: " << kbh.buffer_addr() << "}";
    return os;
}

}  // namespace patronus::hash

namespace std
{
template <>
struct hash<patronus::hash::KVBlockHandle>
{
    std::size_t operator()(const patronus::hash::KVBlockHandle &v) const
    {
        return std::hash<uint64_t>{}(v.remote_addr().val);
    }
};
}  // namespace std
#endif