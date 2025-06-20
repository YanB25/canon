#  Canon

## Abstract

Memory disaggregation is a promising architecture for improving memory utilization in data centers. However, the current disaggregated memory management (MM) systems, which provide alloc/free interfaces to compute nodes, cause free memory to accumulate on the compute side in an unsharable way, leading to severe memory blowup under realworld workloads. This unfortunately contradicts the purpose of memory disaggregation for improving memory utilization. 

We propose CANON, a high-performance disaggregated memory management system that achieves high memory utilization. Contrary to previous systems, CANON performs MM on disaggregated memory (DM) directly, so that the unsharable free memory on the compute side can be mitigated to allow high memory utilization. In realizing such a DM-direct MM strategy, CANON overcomes several associated challenges on performance and fault tolerance: CANON tailors the metadata layout and streamlines concurrency to be performant and proposes a series of designs to prevent client failures from introducing memory leaks and system blocking. Evaluation across five real-world workloads shows that CANON reduces memory waste by 6.1×-71× and tolerates client failures with a modest performance overhead of 3 % to 5 % on average.

## Requirements

- Hardware: Mellanox ConnectX-5 or ConnectX-6 NIC
    - Other models that use the mlx5 driver may also be supported but subject to further testing.
- Drivers: MLNX_OFED 4.9-7.1.0.0 installation
    - We only support the driver version of 4.9 (i.e., MLNX_OFED 4.9-*). Version of 5 is not supported.
- Toolchain: Clang compiler (clang++-10 is recommended)

## Prerequisite

### Configure memcached

Your host machine should have `memcached` installed.

modify [memcached.conf](memcached.conf) to set ip and port of your memcached correctly.

For example,

```
10.0.2.132
2378
```

### Configure the members of the cluster

modify [script/config.py](script/config.py) to set up the `ip` and `numa_id` of available nodes.

For example,

``` python
NODES = [
    # 'IP', numa_id
    ('10.0.2.130', 0),
    ('10.0.2.130', 1),
    ('10.0.2.132', 0),
    ('10.0.2.132', 1),
    ('10.0.2.137', 0),
]
```

Please make sure that your host machine can `ssh` into all these IPs as root without shell interaction.

You can do this by running:

``` shell
ssh-copy-id root@10.0.2.130
```

### Install Dependencies

Then, run the following codes to setup the environment.

``` bash
cd script
./bootstrap.py
```

### Build the project

Please use `clang` to build this project.

```
export CC=$(which clang)
export CXX=$(which clang++)
```

Then, build the probject by

``` bash
cd script
./build.py
```

### Configure huge pages

For memory nodes, 160GB huge pages is recommended for the evaluation.

To allocate huge pages, run

``` bash
cd script
./hugepage.py <ip> <GB>
# e.g., 
# ./hugepage.py 10.0.2.134 160
```

## Benchmarking

### Basic Usage

``` bash
cd script
./bench.py <executable> [flags]
# for example, the below codes will run `bench/bench_dsm.cpp` cluster-wide
./bench.py bench_dsm
```

This script will automatically launch the (distributed) process cluster-wise.

After the experiment finishes, the cluster-wise logs will be fetched and located under `script/fetched/` directory. The log will show the performance results (e.g., throughput, memory utilization, fragmentation, etc).

### Reproducing Canon's Results

**For results of hash table (e.g., Figure-7, Figure-8, Figure-10):**

```
cd script/
./bench.py bench_race_avis_trace --canon --block_mb=2 --trace_file=cluster049.csv --dsm_gb=160 --cache_gb=30 --table_util=50 --buddy_nr=60 
```

- `--canon`: whether or not to use Canon or use the default implementation (FUSEE's).
- `--block_mb`: the block size in MB. 
- `--trace_file`: the trace to replay. The trace files are located in [traces/](traces/)
    - available options: `cluster050.csv`, `cluster026.csv`, `cluster049.csv`, `IBM.csv`. 

The experiment result shows both the performance and memory utilization.

**For results of B+ tree (e.g., Figure-9):**

```
./bench.py bench_tree --read_ratio=80 --put_ratio=20 --preload_ratio=20 --z=0 --warmup_ratio=20 --key_space=20000000 --dsm_gb=100
```

- `--read_ratio`: The percentage of read ratio. In range [0, 100].
- `--put_ratio`: The percentage of put ratio (insert ratio). In range [0, 100].
- `--z`: The skewness parameter of Zipfian distribution. If `z` is set to 0, it will use uniform distribution.

**For results of the techniques for the buddy system (e.g., Figure-11, Figure-12, Figure-13):**

```
./bench.py bench_buddy
```

Configure the below code to use different parameters in the experiment

``` c++
    f.configure_thread_nr({1, 4, 8, 16, 32}); // the number of threads
    f.add_option<avis::BuddyMode>("mode", {avis::BuddyMode::kBoundedRand}); // Whether or not to enable bounded randomization (BR). Could be kBoundedRand, kRand, or kSeq
    f.add_option<bool>("post_order", {true}); // Set to `true` to enable "subtree locality" optimization.
```

The experiment will report both the performance and the memory utilization (fragmentation degree).

**For fault tolerance behaviour under the hash table (e.g., Figure-14(a)):**

```
./bench.py bench_race_avis_crash --simulate_crash
```

**For fault tolerance performance of PLS and BP (e.g., Figure-14(b)):**

```
./bench.py bench_avis_gc
```

**For alternative solutions for addressing memory leaks (e.g., Figure-15):**

```
./bench.pyt bench_avis_micro
```