#include <numa.h>

#include <thread>

#include "bench/experiment.h"
#include "gflags/gflags.h"
#include "glog/logging.h"
#include "util/Bitset.h"
#include "util/gflags_def.h"

struct Spec
{
};

class Experiment : public ::bench::LocalExperiment<Spec>
{
public:
    using Base = ::bench::LocalExperiment<Spec>;
    Experiment()
    {
    }
    void benchmark(BLS &bls,
                   const Config &conf,
                   ::bench::StopToken::pointer token,
                   bool) override
    {
        auto &tlc = bls.thread();
        std::ignore = tlc;
        std::ignore = conf;

        while (!token->stop_requested())
        {
            auto size = fast_pseudo_rand_int(1, 3);
            auto r = bs->atomic_random_set_n(size);
            if (!r)
            {
                token->core().request_stop();
            }
            else
            {
                allocated_.current().push_back({*r, size});
            }
            token->complete_task(1);
        }
    }

    void on_start_bench(BLS &bls, const Config &conf) override
    {
        LOG(INFO) << PRE(conf);
        bs = std::make_unique<util::synchronize::Bitset>(2_MB);
        for (auto &vec : allocated_)
        {
            vec.get().clear();
        }
        Base::on_start_bench(bls, conf);
    }

    void on_thread_end_bench(BLS &bls, const Config &conf) override
    {
        Base::on_thread_end_bench(bls, conf);
    }
    void on_end_bench(const ::bench::ResultRecord &res,
                      BLS &bls,
                      const Config &conf) override
    {
        std::vector<std::pair<size_t, size_t>> sum;
        for (auto &vec : allocated_)
        {
            sum.insert(
                std::end(sum), std::begin(vec.get()), std::end(vec.get()));
        }

        check_dup(sum);
        Base::on_end_bench(res, bls, conf);
    }
    void check_dup(const std::vector<std::pair<size_t, size_t>> &vec)
    {
        LOG(INFO) << "begin checking...";
        for (size_t i = 0; i < vec.size(); ++i)
        {
            for (size_t j = i + 1; j < vec.size(); ++j)
            {
                auto &lhs = vec[i];
                auto &rhs = vec[j];
                Buffer lhs_b = {(char *) lhs.first, lhs.second};
                Buffer rhs_b = {(char *) rhs.first, rhs.second};
                bool not_overlap = test_buffer_not_overlapped(lhs_b, rhs_b);
                CHECK(not_overlap) << PRE(lhs_b) << ", " << PRE(rhs_b) << ": "
                                   << PRE(bs->explain(lhs.first)) << ", "
                                   << PRE(bs->explain(rhs.first));
            }
        }
        LOG(INFO) << "PASS";
    }

private:
    std::unique_ptr<util::synchronize::Bitset> bs;

    Perthread<std::vector<std::pair<size_t, size_t>>> allocated_;
};

int main(int argc, char *argv[])
{
    init_gflags(argc, argv);

    // {
    //     ::bench::ConfigFactory f;
    //     f.configure_thread_nr({16});
    //     // f.add_option_df("reader_rate", {0, 50, 100});
    //     // f.add_option_df("lock", {"rw", "ticket", "mcs", "rp"});
    //     // f.add_option_df("lock", {"rp"});

    //     Experiment exp;
    //     exp.configure_monitor(100us, 10);
    //     exp.launch(f.generate_configs());
    // }

    {
        util::synchronize::Bitset bs(64);
        LOG(INFO) << util::pre_bin(bs.read_slot(0));
        auto a = *bs.atomic_random_set_n(2);
        LOG(INFO) << "get 2: " << util::pre_bin(bs.read_slot(0));
        auto b = *bs.atomic_random_set_n(3);
        LOG(INFO) << "get 3: " << util::pre_bin(bs.read_slot(0));
        auto c = *bs.atomic_random_set_n(2);
        LOG(INFO) << "get 2: " << util::pre_bin(bs.read_slot(0));
        auto d = *bs.atomic_random_set_n(3);
        LOG(INFO) << "get 3: " << util::pre_bin(bs.read_slot(0));
        auto e = *bs.atomic_random_set_n(2);
        LOG(INFO) << "get 2: " << util::pre_bin(bs.read_slot(0));

        bs.atomic_unset_n(b, 3);
        LOG(INFO) << "free b: " << util::pre_bin(bs.read_slot(0));
        bs.atomic_unset_n(c, 2);
        LOG(INFO) << "free c: " << util::pre_bin(bs.read_slot(0));
        bs.atomic_unset_n(a, 2);
        LOG(INFO) << "free a: " << util::pre_bin(bs.read_slot(0));
        bs.atomic_unset_n(e, 2);
        LOG(INFO) << "free e: " << util::pre_bin(bs.read_slot(0));
        bs.atomic_unset_n(d, 3);
        LOG(INFO) << "free d: " << util::pre_bin(bs.read_slot(0));
        // bs.atomic_unset_n(d, 3);
        // LOG(INFO) << "double free d: " << util::pre_bin(bs.read_slot(0));
    }

    LOG(INFO) << "PASS.";
}