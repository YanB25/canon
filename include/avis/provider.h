#pragma once

#include "GlobalAddress.h"

namespace avis
{
struct BuddyProvider
{
    uint32_t node_id;
    GlobalAddress meta_raddr;
    size_t meta_size;
    GlobalAddress buf_raddr;
    size_t buf_size;
    size_t page_size;
};

}  // namespace avis