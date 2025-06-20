#pragma once
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>

#include "util/History.h"

namespace rdma
{
struct Record
{
    ibv_exp_send_wr wr;
    // std::string key;
    // std::string op;
};

extern util::TL_History<Record> history;

}  // namespace rdma