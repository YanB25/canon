#!/usr/python3
import subprocess
import os
import util

NODES = [
    # 'IP', numa_id
    # ('10.0.2.131', 0),
    ('10.0.2.130', 0),
    ('10.0.2.130', 1),

    ('10.0.2.132', 0),
    ('10.0.2.132', 1),

    ('10.0.2.134', 0),
    # ('10.0.2.134', 1),
    # ('10.0.2.135', 0),
    # ('10.0.2.135', 1),

    # ('10.0.2.137', 0),
]
MACHINE_NR = len(NODES)

# memcached
MEMC_CONF = "../memcached.conf"

HUGEPAGE_GB = 120


def get_memcached_conf():
    with open(MEMC_CONF) as f:
        lines = f.read().strip().split()
        ip = lines[0]
        port = lines[1]
        if not util.is_valid_ip(ip) or not util.is_valid_port(port):
            util.error(
                f"Failed to load correct memcached address. Got {ip}:{port} from {MEMC_CONF}")
            return "", ""
        return ip, port

# TWITTER_TRACE_DIR = "/home/yanbin/cache-trace/samples/dist"
TWITTER_TRACE_DIR = "/home/yanbin/cache-trace/full"

# The directory. Don't change.
BASE_DIR = "/home/yanbin/workspace"
CUR_DIR = "./"

BIN_DIR = os.path.join(BASE_DIR, "bin")
RESULT_DIR = os.path.join(BASE_DIR, "result")
ARTIFACTS_DIR = os.path.join(BASE_DIR, "artifacts")

FETCH_DIR = os.path.join(CUR_DIR, "fetched")

PROJ_DIR = os.path.join(CUR_DIR, "../")
TRACES_DIR = os.path.join(PROJ_DIR, "traces")
BUILD_DIR = os.path.join(PROJ_DIR, "build")
THIRD_PARTY_DIR = os.path.join(PROJ_DIR, "thirdparty")

# used to disable annoying messages and warnings
SSH_FLAGS = ['-tt', '-o LOGLEVEL=QUIET']

# BUILD_FAILED_MSG = "recipe for target 'all' failed"
BUILD_FAILED_MSG = "recipe for target"
COREDUMP_MSG = "Notice: 2 systemd-coredump@.service units are running, output may be incomplete"

RNIC_VERSION = 5

FORCE_CROSS_NUMA = False
NUMA_NR = 2


def select_rnic(numa_id: 'str|int'):
    select_numa_id = int(numa_id)
    if FORCE_CROSS_NUMA:
        select_numa_id = (select_numa_id + 1) % NUMA_NR
    return f"mlx{RNIC_VERSION}_{select_numa_id}"


CC = "clang-10"
CXX = "clang++-10"

DEBUG_SCRIPT = False

if __name__ == '__main__':
    import pprint
    pprint.pprint(NODES)
    print(f"MACHINE_NR: {MACHINE_NR}")
    print(f"CC {CC}")
    print(f"CXX {CXX}")
    print(f"NUMA_NR {NUMA_NR}")
