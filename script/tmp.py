#!/usr/bin/python3
import subprocess
import sys
import config
import cluster
import util
from cluster import Cluster
import pprint
import os
import pm
import time

# dist = [(0.8, 8), (0.2, 16), (233, 0.59 / 6), (466, 0.59/6), (0.01, 1 * 1024 * 1024)]
dist = [(8, 0.2), (16, 0.4)]
acc = 0.4
for i in range(6):
    cur = 0.59 / 6
    dist.append((233 * (i + 1), acc + cur))
    acc += cur
for i in range(6):
    cur = 0.01 / 6
    dist.append((170 * 1024 * (i + 1), acc + 0.01 / 6))
    acc += cur
dist.append((1 * 1024 * 1024, 1.0))

for (val, p) in dist:
    print(f"{p}, {val}")

# pprint.pprint(dist)

# percentile = [i * 0.01 for i in range(100)]

# for p in percentile:

