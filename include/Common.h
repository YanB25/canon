#pragma once
#ifndef __COMMON_H__
#define __COMMON_H__

#include <glog/logging.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>

#include "util/Literals.h"

using namespace util::literals;

constexpr static size_t MAX_MACHINE = 10;
constexpr static int NR_DIRECTORY = 1;
// dont used up all
// NOTE: this number is PER-NUMA.
// a) NR_DIRECTORY may use threads (use NR_DIRECTORY)
// b) the performance monitor may use threads (use 1)
// c) the worker may use threads (use 1)
// leave at least @cores_per_numa - NR_DIRECTORY - 1
constexpr static ssize_t kCorePerNuma = 36;
constexpr static ssize_t kMaxWorker = 1;
constexpr static ssize_t kMaxAppThread =
    kCorePerNuma - NR_DIRECTORY - 1 - kMaxWorker;
constexpr static const char *kNICName = "eno1";
constexpr static size_t kServerStartEpId = kCorePerNuma - kMaxWorker;

#define MESSAGE_SIZE 128  // byte
#define RAW_RECV_CQ_COUNT 128
#define MAX_SEND_WQE_INLINE_KLMS (20)  // query the device
#define MAX_UMR_RECURSION_DEPTH (4)    // query the evice
#define APP_MESSAGE_NR 96
#define DIR_MESSAGE_NR 128

using trace_t = uint8_t;

namespace config
{
constexpr static size_t kDefaultDSMSize = 30_GB;
constexpr static size_t kDefaultCacheSize = 35_GB;

// it seems that vectorization gets slower.
constexpr static bool kUseAVXMemcpy = false;
constexpr static bool kUseSSEMemcpy = false;

// If @kEnableAbrtHandler is true,
// we register a handler to SIGABRT, which do the cleanup
// or any registered task before process exits
constexpr static bool kEnableAbrtHandler = true;

namespace redn
{
constexpr static size_t kMasterDir = 0;
constexpr static size_t kWorkerDir = 1;
constexpr static size_t kFollowerDir = 2;
}  // namespace redn

}  // namespace config

namespace define
{
constexpr uint16_t kCacheLineSize = 64;

// for remote allocate
constexpr uint64_t kChunkSize = 16_MB;

// lock on-chip memory
constexpr uint64_t kLockStartAddr = 0;
constexpr uint64_t kLockChipMemSize = 256 * 1024;
// Just one 1/4, yes, you could use all of them.
// constexpr uint64_t kLockChipMemSize = 64_KB;

constexpr uint16_t kMaxCoroNr = 32;

// for dsm
constexpr static uint32_t kRDMABufferSize =
    ::config::kDefaultCacheSize / kMaxAppThread;
constexpr int64_t kPerCoroRdmaBuf = 32_KB;
}  // namespace define

// For Tree
using Key = uint64_t;
using Value = uint64_t;
constexpr Key kKeyMin = std::numeric_limits<Key>::lowest();
constexpr Key kKeyMax = std::numeric_limits<Key>::max();
constexpr Value kValueNull = 0;
constexpr uint32_t kInternalPageSize = 1024;
constexpr uint32_t kLeafPageSize = 1024;

namespace config
{
static const std::array<size_t, 2> __kServerNodeIds{2, 3};
static const std::array<size_t, 6> __kClientNodeIds{0, 1, 4, 5, 6, 7};
inline bool is_server(size_t nid)
{
    auto it =
        std::find(__kServerNodeIds.cbegin(), __kServerNodeIds.cend(), nid);
    return it != __kServerNodeIds.cend();
}
inline bool is_client(size_t nid)
{
    auto it =
        std::find(__kClientNodeIds.cbegin(), __kClientNodeIds.cend(), nid);
    return it != __kClientNodeIds.cend();
}
inline auto get_client_nids()
{
    return __kClientNodeIds;
}
inline auto get_server_nids()
{
    return __kServerNodeIds;
}
constexpr static size_t server_node_nr()
{
    return __kServerNodeIds.size();
}
constexpr static size_t client_node_nr()
{
    return __kClientNodeIds.size();
}
constexpr static size_t handle_client_node_nr_per_server()
{
    return (client_node_nr() + server_node_nr() - 1) / server_node_nr();
}

// about enabling monitors, sacrifying performance
constexpr static bool kMonitorControlPath = false;
constexpr static bool kMonitorReconnection = false;
constexpr static bool kMonitorFailureRecovery = false;
constexpr static bool kMonitorAddressConversion = false;
constexpr static bool kMonitorLeaseContext = false;
constexpr static bool kMonitorCoroSwitch = false;
constexpr static bool kMonitorAccessDistribution = true;

constexpr static bool kReportTraceViewRoute = false;

constexpr static bool kEnableReliableMessageSingleThread = true;
constexpr static bool kEnableSkipMagicMw = true;

constexpr static bool kEnableRdmaTrace = false;
// constexpr static double kRdmaTraceRateGet = 1.0 / 200_K;
// constexpr static double kRdmaTraceRatePut = 1.0 / 20_K;
// constexpr static double kRdmaTraceRateDel = 1.0 / 20_K;
// constexpr static double kRdmaTraceRateExpand = 1.0;
constexpr static double kRdmaTraceRateBootstrap = 1.0 / 20_K;
constexpr static double kRdmaTraceRateGet = 0;
constexpr static double kRdmaTraceRatePut = 0;
constexpr static double kRdmaTraceRateDel = 0;
constexpr static double kRdmaTraceRateExpand = 0;
// constexpr static double kRdmaTraceRateBootstrap = 0;

// about opening a feature
constexpr static bool kEnableReliableMessage = true;

// about higher level of debugging, sacrifying performance.
constexpr static bool kEnableValidityMutex = false;
constexpr static bool kEnableTrace = false;
constexpr static uint64_t kTraceRate =
    100000;  // (1.0 / kTraceRate) possibility
// slab allocator checks whether each free is valid.
// Turn this off, since patronus allows free-ing buffers across clients
constexpr static bool kEnableSlabAllocatorStrictChecking = false;
constexpr static bool kMonitorSlabAllocator = false;
// other settings
constexpr static size_t kLeaseCacheItemLimitNr = 3;
}  // namespace config

#endif /* __COMMON_H__ */