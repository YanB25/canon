#!/usr/bin/python3
import math
import numpy as np

def gen(begin, e, max_val, head=[]):
    ls = head
    cur = begin
    ls.append(cur)
    while cur <= max_val:
        cur = math.ceil(cur * e)
        if cur == ls[-1]:
            cur += 1
            continue
        if cur <= max_val:
            ls.append(cur)
    # cur = math.ceil(cur * e)
    ls.append(cur)
    return ls

def gen_head(begin, end, step):
    ls = []
    cur = begin
    while cur < end:
        ls.append(cur)
        cur += step
    return ls

def fragment(ls):
    f = []
    for i in range(len(ls) - 1):
        cur = ls[i]
        next = ls[i + 1]
        if cur >= next:
            print(f"ERROR: {cur}, {next}")
        frag = (next -1 - cur) / next
        f.append(frag)
    return f


very_head = [16]
head = gen_head(32, 128, 8)
ls = gen(begin=128, e=1.05, max_val=16 * 1024 * 1024, head=(very_head + head))
# ls = gen(begin=8, e=1.05, max_val=16 * 1024 * 1024, head=[])
fragments = fragment(ls)

# print(fragments)
max_frag = np.max(fragments)
print(max_frag)

print()
print()
print(f"// This array is generated from script/gen_slab.py")
print(f"std::array<size_t, {len(ls)}> size_class_{{", end='')
# for item in ls:
ls = [str(i) for i in ls]
print(', '.join(ls))
print('};')

