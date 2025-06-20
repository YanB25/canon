#!/usr/bin/python3
import util
import pm
import config

src_files = util.listdir_recursive("../src", "*.cpp", True)
hdr_files = util.listdir_recursive("../include", "*.h", True)

def check(file):
    p = pm.launch_local_process("clang-tidy")
    p.run(f"clang-tidy {file} -p {config.BUILD_DIR} ")
    p.commit()
    p.wait()
    
    
if __name__ == '__main__':
    for src in src_files:
        check(src)
