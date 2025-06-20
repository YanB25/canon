#!/usr/bin/python3
import hooks
import config
import cluster
import sys

silent = not config.DEBUG_SCRIPT

if len(sys.argv) <= 1:
    print("{} bench_file [flags]".format(sys.argv[0]))
    exit(-1)
binary = sys.argv[1]
params = sys.argv[2:]

# HOOKS = hooks.valgrind()
# HOOKS = hooks.valgrind(full_leak_check=True)
# HOOKS = ['gdb', '-ex run', '--args']
HOOKS = []

c = cluster.Cluster(config.NODES)
c.local_benchmark(binary, params, hooks=HOOKS, silent=silent)
