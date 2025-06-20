#!/usr/bin/python3
import sys
import pm

NODES = [
    '10.0.2.130',
    '10.0.2.131',
    '10.0.2.132',
    '10.0.2.134',
    '10.0.2.136',
    '10.0.2.137',
]

proc = []
for node in NODES:
    p = pm.launch_remote_process_inline(node, node, sys.argv[1:])
    proc.append(p)

for p in proc:
    print(f"============== {p.node()} =============")
    p.wait()
    print()
