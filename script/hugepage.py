#!/usr/bin/python3

import config
import cluster
import sys

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"{sys.argv[0]} <ip> <hugepage GB>")
        print(f"e.g., {sys.argv[0]} 127.0.0.1 32")
        exit()

    c = cluster.Cluster(config.NODES)
    ip = sys.argv[1]
    gb = int(sys.argv[2])
    number = int(gb * 1024 / 2)
    c.node_execute_short_cmd(
        ip, ["echo", str(number), "> /proc/sys/vm/nr_hugepages"], silent=False
    )