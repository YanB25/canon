#pragma once
#include "gflags/gflags.h"
#include "gflags_dec.h"
#include "glog/logging.h"

DEFINE_string(exec_meta, "unknown", "The meta data of this execution");
DEFINE_uint32(machine_nr, 1, "The number of machines");
DEFINE_string(rnic, "mlx5_0", "The name of the used RNIC");
DEFINE_bool(no_csv, false, "Whether or not disable CSV results");
DEFINE_string(binary, "", "The name of current binary");

void init_gflags(int &argc, ArgvT &argv);