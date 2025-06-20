#include <atomic>
#include <cinttypes>
#include <thread>

#include "Pool.h"
#include "Timer.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Pre.h"
#include "util/Rand.h"
#include "util/TLS.h"
#include "util/Util.h"
#include "util/gflags_def.h"

extern "C"
{
#include "infiniband/mlx5dv.h"
}
DEFINE_string(msg, "hello workd", "the message");

struct Bar
{
    int a;
    std::atomic<int> b;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    std::queue<Bar *> objs;

    ChronoTimer timer;
    size_t op = 100_M;

    [[maybe_unused]] auto &allocator = TLS<DPool<Bar>>();

    for (size_t i = 0; i < op; ++i)
    {
        auto rm = fast_pseudo_bool_with_prob(0.5);
        if (rm && !objs.empty())
        {
            auto *o = objs.front();
            allocator.free(o);
            // free(o);
            objs.pop();
        }
        else
        {
            auto *obj = allocator.alloc();
            // auto *obj = (Bar *) malloc(sizeof(Bar));
            objs.push(obj);
        }
    }
    auto ns = timer.pin();

    LOG(INFO) << "ops: " << util::pre_ops(op, ns) << " in " << util::pre_ns(ns);

    LOG(INFO) << PRE(TLS<DPool<Bar>>().total_size());

    LOG(INFO) << "PASS.";
}