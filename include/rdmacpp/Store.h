#pragma once

#include <memory>
#include <string>

namespace rdma
{
class Store
{
public:
    using pointer = std::shared_ptr<Store>;
    virtual void put(const std::string &key, const std::string &value) = 0;
    virtual std::string get(const std::string &key) = 0;
    virtual ~Store() = default;
};
}  // namespace rdma