#pragma once
#include <cstddef>
#include <cstdint>

namespace config::verbose
{
constexpr static size_t kBenchReserve_1 = 1;
constexpr static size_t kBenchReserve_2 = 2;
constexpr static size_t kSystem = 3;
constexpr static size_t kUserApp_1 = 4;
constexpr static size_t kUserApp_2 = 5;
constexpr static size_t kRdmaAdpt = 6;
constexpr static size_t kCoroLauncher = 7;
constexpr static size_t kPatronus = 8;
constexpr static size_t kPatronusUtils = 9;
constexpr static size_t kDSM = 10;
constexpr static size_t kUmsg = 11;
constexpr static size_t kRdmaOperation = 12;
constexpr static size_t kDump = 13;

constexpr static size_t kTimeSyncer = kRdmaOperation;

constexpr static size_t kVerbose = 20;
}  // namespace config::verbose