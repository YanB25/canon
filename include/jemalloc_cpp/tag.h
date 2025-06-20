#pragma once
#include <iostream>
namespace Jemalloc
{
enum class Tag
{
    DSM,
    RDMA_Buf,
    Test,
};
inline std::ostream &operator<<(std::ostream &os, Tag tag)
{
    switch (tag)
    {
    case Tag::DSM:
    {
        os << "Tag::DSM";
        break;
    }
    case Tag::RDMA_Buf:
    {
        os << "Tag::RDMA_Buf";
        break;
    }
    case Tag::Test:
    {
        os << "Tag::Test";
        break;
    }
    default:
    {
        os << "Tag::Unkown(" << (int) tag << ")";
    }
    }
    return os;
}
}  // namespace Jemalloc