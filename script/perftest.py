#!/usr/bin/python3
import cluster
import config
import time
import pm

HOOK=""
PORT=18515
SERVER="10.0.2.134"
CLIENTS=["10.0.2.132", "10.0.2.133"]
NR_INSTANCES = 4

class Config:
    def __init__(self, exec="ib_write_bw"):
        self.exec = exec
        self.params = {}
        self.params["qp"] = 8
        self.params["size"] = 65536
        self.params["post_list"] = 1
        self.params["tx-depth"] = 128
        self.flags = ["--noPeak", "--report_gbits", "-D 5"]

    def cmd_str(self):
        cmd = []
        for k, v in self.params.items():
            cmd.append(f"--{k}={v}")
        cmd += self.flags
        cmd = ' '.join(cmd)
        cmd = f"{self.exec} {cmd}"
        return cmd


if __name__ == '__main__':
    c = cluster.Cluster(config.NODES)
    killall_cmd = ["killall", "ib_write_bw"]
    c.cluster_execute_cmd(killall_cmd, True)

    config = Config("ib_write_bw")
    print(config.cmd_str())

    procs = set()
    nr = 0
    for idx, client in enumerate(CLIENTS):
        for ith in range(NR_INSTANCES):
            p = pm.launch_remote_process_inline(f"server-{idx}-{ith}", SERVER, [f"{config.cmd_str()} --port={PORT + nr}"], silent=True)
            nr += 1
            procs.add(p)
    time.sleep(1)

    nr = 0
    for idx, client in enumerate(CLIENTS):
        for ith in range(NR_INSTANCES):
            p = pm.launch_remote_process_inline(f"client-{idx}-{ith}", client, [f"{config.cmd_str()} {SERVER} --port={PORT + nr}"], silent=False)
            nr += 1
            procs.add(p)
    
    for p in procs:
        p.wait()


    # kill all processes
    cmd = ["killall", "ib_write_bw"]
    c.cluster_execute_cmd(killall_cmd, True)
