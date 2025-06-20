#!/usr/bin/python3
import util
import pm
import config
import os

src_files = util.listdir_recursive("../src", "*.cpp", True)
hdr_files = util.listdir_recursive("../include", "*.h", True)

style_file = os.path.join(config.PROJ_DIR, ".clang-format")

def format(file):
    print(file)
    p = pm.launch_local_process("clang-format")
    p.run(f"clang-format -i -style=file:{style_file} {file}")
    p.commit()
    return p

def format_batch(files, batch_size):
    while len(files) > 0:
        cur = files[:batch_size]
        files = files[batch_size:]

        processes = set()
        for file in cur:
            p = format(file)
            processes.add(p)
        
        for p in processes:
            p.wait()

if __name__ == '__main__':
    format_batch(src_files, 32)
    format_batch(hdr_files, 32)