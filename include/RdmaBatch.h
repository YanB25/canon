#pragma once

#include <Rdma.h>
#include <infiniband/verbs.h>
#include <infiniband/verbs_exp.h>

#include <cinttypes>
#include <cstddef>

#include "CoroContext.h"
#include "GlobalAddress.h"
#include "rdmacpp/QP.h"
#include "rdmacpp/WRCtx.h"
#include "util/RetCode.h"
#include "util/Tracer.h"

class RdmaOperationBatch
{
public:
    RdmaOperationBatch() = default;
    void add(rdma::QP *qp)
    {
        qps_.insert(qp);
    }
    const auto &qps() const
    {
        return qps_;
    }
    void clear()
    {
        qps_.clear();
    }

private:
    std::unordered_set<rdma::QP *> qps_;
};