#pragma once
#ifndef __DIRECTORY_H__
#define __DIRECTORY_H__

#include <thread>
#include <unordered_map>

#include "Common.h"
#include "Connection.h"
#include "GlobalAllocator.h"
#include "memory/block_allocator.h"
#include "memory/refill_allocator.h"

class Directory
{
public:
    using Pointer = std::shared_ptr<Directory>;
    Directory(DirectoryConnection &dCon,
              const std::vector<RemoteConnection> &remoteInfo,
              uint32_t machineNR,
              uint16_t dirID,
              uint16_t nodeID,
              size_t bind_core,
              void *dsm_base_addr,
              ::mem::BaseAllocator::pointer alloc);
    static std::shared_ptr<Directory> newInstance(
        DirectoryConnection &dCon,
        const std::vector<RemoteConnection> &remoteInfo,
        uint32_t machineNR,
        uint64_t dirID,
        uint16_t nodeID,
        size_t bind_core,
        void *dsm_base_addr,
        ::mem::BaseAllocator::pointer alloc);

    ~Directory();
    void signal_exit()
    {
        exit_ = true;
    }

private:
    std::atomic<bool> exit_{false};
    DirectoryConnection &dCon;
    const std::vector<RemoteConnection> remoteInfo;

    uint32_t machineNR;
    uint16_t dirID;
    uint16_t nodeID;
    size_t bind_core_;

    void *dsm_base_addr_{};
    ::mem::BaseAllocator::pointer alloc_;

    std::thread dirTh;

    // GlobalAllocator::pointer chunckAlloc;

    void dirThread();

    void sendData2App(const RawMessage *m);

    void process_message(const RawMessage *m);
};

#endif /* __DIRECTORY_H__ */
