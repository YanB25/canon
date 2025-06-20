import sys
import pm2 as pm
import asyncio
import config
import errno
import pandas as pd
import itertools
import pprint

TO_CSV = "fetched/bench_loop.csv"

TRACES = ["cluster026.csv", "cluster050.csv", "cluster049.csv"]

TRACE_PARAMS = {
    'cluster026.csv': {
        'table_util': 20 * 5
    },
    'cluster050.csv': {
        'table_util': 10
        
    },
    'cluster049.csv': {
       'table_util': 20 
    },
    "ETC.csv": {
        'table_util': 100
    },
    "IBM.csv": {
        'table_util': 5
    },
    'null': {}
}

DEFAULT_PARAMS = {
    'buddy_nr': 60,
    'dsm_gb': 160,
    'cache_gb': 30,
    'block_mb': 1,
    'thread_nr': 32,
    'coro_nr': 2,
    'canon': 'false',
}

async def cleanup():
    print(f"[system] cleanup...")
    proc = await pm.create_remote_process('cleanup')
    await proc.run('cd /home/yanbin/sherman/script')
    await proc.run(f"stdbuf -oL -eL ./cleanup.py 2>&1")
    await proc.run('exit')
    return True
    
def print_full(x):
    pd.set_option('display.max_rows', None)
    pd.set_option('display.max_columns', None)
    print(x)
    pd.reset_option('display.max_rows')
    pd.reset_option('display.max_columns')

def product_dict(**kwargs):
    keys = kwargs.keys()
    for instance in itertools.product(*kwargs.values()):
        yield dict(zip(keys, instance))

class Ctx:
    def __init__(self):
        self.results = []
    
    def add_results(self, result):
        self.results.append(result)
        df = self.to_df()
        print_full(df)
        df.to_csv(TO_CSV, index=None)
    
    def to_df(self):
        df = pd.DataFrame(self.results)
        return df

async def run_task(ctx, binary: 'str', params):
    print(f"[system] Waiting for several seconds before launching next expr...")
    await asyncio.sleep(3)
    print(f"[system] run_task with params {params}")
    proc = await pm.create_remote_process('main')
    await proc.run('cd /home/yanbin/sherman/script')
    # await proc.run('ls')
    param_str = " ".join(f"--{key}={value}" for key, value in params.items())
    await proc.run(f"stdbuf -oL -eL ./bench.py {binary} {param_str} 2>&1")
    await proc.run('exit')
    table_capacity = 0
    table_util = 0
    client_holding = 0
    bitmap_occupies = 0
    cluster_ops = 0
    long_wait = 0
    while True:
        line = await proc.read_line()
        if line is None:
            break
        if "Table capacity" in line:
            try:
                table_capacity = line.strip().split()[7] 
                table_util = line.strip().split()[11]
            except Exception as e:
                print(f"[system] Failed to parse {line}")
        if "Client holding" in line:
            try:
                client_holding = line.strip().split()[6]
            except Exception as e:
                print(f"[system] Failed to parse {line}: {e}")
        if "Bitmap occupies" in line:
            try:
                bitmap_occupies = line.strip().split()[6]
            except Exception as e:
                print(f"[system] Failed to parse {line}: {e}")
        if "results: cluster ops:" in line:
            try:
                cluster_ops = line.strip().split()[7]
            except Exception as e:
                print(f"[system] Failed to parse {line}: {e}")
        if "Barrier for a long time" in line:
            long_wait += 1
            if long_wait >= 6:
                print(f"[system] Long waiting detected. Skip this: {params}")
                print(f"[system] This is detected by {line}")
                try:
                    await proc.kill()
                except:
                    pass
                return False
        if "Timeout: 32 s" in line or "Operation failed:" in line or "Deadlock at" in line or "Failed to alloc huge page" in line:
            print(f"[system] Long waiting detected. Skip this: {params}")
            print(f"[system] This is detected by {line}")
            try:
                await proc.kill()
            except:
                pass
            return False
    await proc.wait()

    # df = pd.read_csv('fetched/10.0.2.130.0.csv')
    # print_full(df)
    results = {**params, 
               'table_capacity (GB)': table_capacity, 
               'client_holding (GB)': client_holding, 
               "bitmap_occupies (GB)": bitmap_occupies, 
               "cluster_ops": cluster_ops,
               "table_util (%)": table_util,
               }
    print(f"[system] before ctx.add_results...")
    ctx.add_results(results)
    print(f"[system] before ctx.add_results DONE.")

    await cleanup()
    return True
    

async def main(ctx, binary: 'str', traces: 'List[str]'):
    params_dict = {
        'thread_nr': [32],
        'coro_nr': [2],
        # 'coro_nr': [3, 4, 8, 16],
        # 'coro_nr': [1, 2, 3, 4, 6, 8],
        # 'canon': ["false", "true"],
        'canon': ["false"],
        # 'block_mb': [1, 2, 4, 8, 16, 32, 64],
        'block_mb': [1],
    }
    for trace in traces:
        params_iter = product_dict(**params_dict)
        for params in params_iter:
            run_params = {**DEFAULT_PARAMS, **TRACE_PARAMS[trace], **params, 'trace_file': trace}
            pprint.pprint(run_params)
            first_time_ok = await run_task(ctx, binary, run_params)
            if not first_time_ok:
                print(f"[system] Failed. Retry another time for {params}")
                second_time_ok = await run_task(ctx, binary, params)
                print(f"[system] The second try: {second_time_ok} for {params}")
            

if __name__ == '__main__':
    binary = "bench_race_avis_trace"
    # traces = ['ETC.csv']
    # traces = ['IBM.csv']
    traces = ['cluster026.csv']
    # traces = ['cluster026.csv', 'cluster049.csv', 'cluster050.csv', 'ETC.csv']
    # binary = "bench_race_avis_util"
    # traces = ['null']
    ctx = Ctx()

    asyncio.run(main(ctx, binary, traces))

    print()
    df = ctx.to_df()
    print_full(df)
    df.to_csv("fetched/bench_loop.csv", index=None)

    asyncio.run(cleanup())