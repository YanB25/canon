#!/usr/bin/python3
import config
import cluster
import sys

c = cluster.Cluster(config.NODES)
files = sys.argv[1:]
c.deploy_sync(files, silent=not config.DEBUG_SCRIPT)
# c.deploy_sync(files, silent=False)
