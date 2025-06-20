# Experiment

This module, `experiment`, provides a handy and extendable framework for you to launch any experiments and get the results.

The goals of this module is to do all the dirty things for you that is *necessary* but *unrelated* to the experiment itself, so that you can *focus* on your experiment logic in each benchmark file.

This module does the following things for you

- launch threads (and coroutines) and bind them to the NUMA and CPU cores
- synchronize threads (across machine or cluster)
- gathering *accurate* experimental results (across machine or cluster)
- declare your customized configuration and evaluate against the configuration list
- dumps the results to CSV format

## Quick Starts

Following is a minimal example that spawns 8 threads for the evaluation.

Assume we want to benchmark how `std::atomic<uint64_t>::fetch_add(1)` performs.

``` c++
// (1) per-thread objects
struct ThreadLocalStore
{
    // ...
};

class Experiment: public bench::LocalExperiment<ThreadLocalStore> // (2)
{
public:
    // (1)
    void benchmark(ThreadCtx& tls, 
                   const Config& conf, // (4)
                   StopToken::pointer token, 
                   bool is_master) override
    {
        while (likely(!token->stop_requested()))
        {
            // the operation to benchmark here
            atm_.fetch_add(1);
            // 
            token->complete_task(1);
        }
    }
private:
    std::atomic<uint64_t> atm_;
};

int main()
{
    Experiment exp;
    bench::ConfigFactory f;
    f.configure_thread({8}); // run with 8 threads
    exp.launch(f.generate_configs());
}
```

The whole benchmark logic, i.e., `std::atomic<uint64_t>::fetch_add(1)`, is placed in the *override* `benchmark` function, see `(1)`.

