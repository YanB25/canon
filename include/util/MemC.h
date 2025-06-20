#pragma once
#include <libmemcached/memcached.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>

#include "glog/logging.h"
#include "util/CRTP.h"

class MemC : public util::MakeUnique<MemC>
{
public:
    MemC()
    {
        memcached_return rc;

        std::ifstream conf("../memcached.conf");
        memcached_server_st *servers{};

        if (!conf)
        {
            LOG(FATAL) << "can't open memchaced.conf at ../memcached.conf";
        }

        std::getline(conf, addr_);
        std::getline(conf, port_);

        global_memc_ = memcached_create(NULL);
        servers = memcached_server_list_append(
            servers, trim(addr_).c_str(), std::stoi(trim(port_)), &rc);

        rc = memcached_server_push(global_memc_, servers);

        free(servers);
        LOG_IF(FATAL, rc != MEMCACHED_SUCCESS)
            << "Can't add server: " << memcached_strerror(global_memc_, rc);
        memcached_behavior_set(
            global_memc_, MEMCACHED_BEHAVIOR_BINARY_PROTOCOL, 1);
    }
    ~MemC()
    {
    }
    template <typename T>
    void put(const std::string &key,
             const T &value,
             std::chrono::nanoseconds ns)
    {
        memcached_return rc;
        while (true)
        {
            rc = memcached_set(global_memc_,
                               key.c_str(),
                               key.length(),
                               (const char *) &value,
                               sizeof(value),
                               (time_t) 0,
                               (uint32_t) 0);
            if (rc == MEMCACHED_SUCCESS)
            {
                return;
            }
            std::this_thread::sleep_for(ns);
        }
    }
    template <typename T>
    std::optional<T> try_get(const std::string &key,
                             std::chrono::nanoseconds ns)
    {
        size_t size;
        char *res;
        uint32_t flags;
        memcached_return rc;

        while (true)
        {
            res = memcached_get(
                global_memc_, key.c_str(), key.length(), &size, &flags, &rc);
            if (rc == MEMCACHED_SUCCESS || rc == MEMCACHED_NOTFOUND)
            {
                break;
            }
            std::this_thread::sleep_for(ns);
        }

        if (rc == MEMCACHED_NOTFOUND)
        {
            return std::nullopt;
        }
        else
        {
            T ret;
            memcpy(&ret, res, std::min(sizeof(T), size));
            free(res);

            return ret;
        }
    }
    template <typename T>
    T get(const std::string &key, std::chrono::nanoseconds ns)
    {
        while (true)
        {
            auto opt = try_get<T>(key, ns);
            if (opt)
            {
                return *opt;
            }
        }
    }

private:
    std::string addr_;
    std::string port_;
    memcached_st *global_memc_{};

    std::string trim(const std::string &s)
    {
        std::string res = s;
        if (!res.empty())
        {
            res.erase(0, res.find_first_not_of(" "));
            res.erase(res.find_last_not_of(" ") + 1);
        }
        return res;
    }
};