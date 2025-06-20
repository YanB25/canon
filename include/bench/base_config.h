#pragma once
#include <cstddef>
#include <list>
#include <memory>
namespace bench
{
class IBenchConfig
{
public:
    using Pointer = std::shared_ptr<IBenchConfig>;
    virtual size_t thread_nr() const = 0;
    virtual size_t coro_nr() const
    {
        // 0 means no coro
        return 0;
    }
    virtual size_t effective_client_nr() const
    {
        return thread_nr() * std::max((size_t) 1, coro_nr());
    }
    virtual std::string name() const
    {
        return "unknown";
    }
    virtual std::list<std::pair<std::string, std::string>> df_options() const
    {
        return {};
    }
    virtual ~IBenchConfig() = default;
};

}  // namespace bench