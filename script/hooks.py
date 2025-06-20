def valgrind(full_leak_check=False, trace_children=False):
    hooks = ['valgrind']
    if full_leak_check:
        hooks += ['--leak-check=full']
    if trace_children:
        hooks += ['--trace-children=yes']
    return hooks