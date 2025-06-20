#pragma once
#include "gflags/gflags.h"

DECLARE_string(exec_meta);
DECLARE_uint64(numa_id);
DECLARE_string(node_id);
DECLARE_uint32(machine_nr);
DECLARE_string(rnic);
DECLARE_bool(no_csv);
DECLARE_string(binary);

using ArgvT = char **;
void init_gflags(int &argc, ArgvT &argv);
