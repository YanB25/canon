#!/bin/bash
# ./run.sh <script>
. env.sh

set -o xtrace
set -e

cmd=$1
shift

# read $VM_FILE and $INET_FILE into array
declare -a vms=()
declare -a inets=()
while IFS= read -r line
do
    vms+=($line)
done < <(grep -v '^ *#' < "$VM_FILE")

while IFS= read -r line
do
    inets+=($line)
done < <(grep -v '^ *#' < "$INET_FILE")

# enable color and only log to terminal
# NOTE: glog needs TERM env to enable color.
GLOG_FLAGS="GLOG_logtostderr=1 GLOG_colorlogtostderr=1 "
ASAN_FLAGS="ASAN_OPTIONS=halt_on_error=0"

# NOTE: this is necessary
# the indirection will avoid breaking "--v=3 --n=1" into "--v=3" "--v=n1"
param=$@

node_id=${NUMA_NR}
for (( i=1; i<${#vms[@]}; i++ )); do
    for (( numa_id=0; numa_id<$NUMA_NR; numa_id++ )); do
        echo [start service on ${vms[$i]} \(inet ${inets[$i]}\) at numa ${numa_id} ...]
        nohup ssh root@${inets[$i]} "cd ${BIN_DIR}; source /etc/profile; export TERM='linux'; ${GLOG_FLAGS} ${ASAN_FLAGS} unbuffer ${BIN_DIR}/$cmd $param --exec_meta=${exec_meta}.${numa_id} --numa_id=${numa_id} --node_id=${node_id} 1>${WORK_DIR}/${numa_id}.LOG 2>&1" &
        node_id=`expr $node_id + 1`
    done
done


node_id=1
for (( numa_id=1; numa_id<$NUMA_NR; numa_id++ )); do
    echo [start service on ${vms[0]} \(inet ${inets[0]}\) at numa ${numa_id} ...]
    nohup ssh root@${inets[0]} "cd ${BIN_DIR}; source /etc/profile; export TERM='linux'; ${GLOG_FLAGS} ${ASAN_FLAGS} unbuffer ${BIN_DIR}/$cmd $param --exec_meta=${exec_meta}.${numa_id} --numa_id=${numa_id} --node_id=${node_id} 1>${WORK_DIR}/${numa_id}.LOG 2>&1" &
    node_id=`expr $node_id + 1`
done

numa_id=0
node_id=0
./ssh.sh ${inets[0]} "cd ${BIN_DIR}; source /etc/profile; export TERM='linux'; ${GLOG_FLAGS} ${ASAN_FLAGS} unbuffer ${BIN_DIR}/$cmd $param --exec_meta=${exec_meta}.${numa_id} --numa_id=${numa_id} --node_id=${node_id} 2>&1 | tee ../${numa_id}.LOG"

# echo [Waiting peers to finish]
# wait