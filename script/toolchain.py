#!/usr/bin/python3
import pm
import sys
import errno

if len(sys.argv) < 2:
    print(f"{sys.argv[0]} <VERSION>")
    exit(errno.EINVAL)
else:
    VERSION=int(sys.argv[1])

p = pm.launch_local_process("make toolchain")
DIR='../toolchain'
p.run(f"rm -rf {DIR}")
p.run(f"mkdir -p {DIR}")
p.run(f"cd {DIR}")

# https://stackoverflow.com/questions/41962611/how-to-select-a-particular-gcc-toolchain-in-clang
p.run("ln -s /usr/include include") #for headerfiles
p.run("ln -s /usr/bin bin") #for tools like ld
p.run("mkdir -p lib/gcc/x86_64-linux-gnu/") #clang will deduce what to select
p.run("cd lib/gcc/x86_64-linux-gnu/")
#link the toolchain we want here
p.run(f"ln -s /usr/lib/gcc/x86_64-linux-gnu/{VERSION} {VERSION}")

# usage: clang++ --gcc-toolchain=$MYTOOLCHAIN main.cpp

p.commit()
p.wait()