#!/usr/bin/python3
import subprocess
import pm
import os
import config
import time
import fnmatch
import json

def report_time_decorator(func):
    def wrapper_function(*args, **kwargs):
        start = time.time()
        ret = func(*args,  **kwargs)
        end = time.time()
        elaps = end - start
        state("Approximately take {:.4}s".format(elaps))
        return ret
    return wrapper_function

class bcolors:
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'
    UNDERLINE = '\033[4m'


def check_output(cmd: 'list[str]'):
    return subprocess.check_output(cmd).decode('utf-8').strip()


def generate_exec_meta(numa_id):
    commit_hash = check_output(["git", "rev-parse", "HEAD"])
    short_hash = commit_hash[:8]
    date = check_output(["date", "+%Y-%m-%d.%H:%M:%S"])
    exec_meta = f"{date}.{short_hash}.{numa_id}"
    return exec_meta

def report_compile_info_decorator(func):
    def wrapper_function(*args, **kwargs):
        ret = func(*args,  **kwargs)
        report_compile_info()
        return ret
    return wrapper_function

def build_async(cmake_flags: 'list[str]', targets: 'list[str]', silent: 'bool'):
    p = pm.launch_local_process("build", silent)
    p.run("source /etc/profile \n")
    p.run(f"export CC=$(which {config.CC}) \n")
    p.run(f"export CXX=$(which {config.CXX}) \n")
    p.run("mkdir -p ../build \n")
    p.run("cd ../build \n")
    cmake_flags_str = ' '.join(cmake_flags)
    print(f"{cmake_flags_str}")
    p.run(f"cmake {cmake_flags_str} .. \n")

    if targets is not None and len(targets) > 0:
        target_str = "--target " + ' '.join(targets)
    else:
        target_str = ""
    p.run(f"cmake --build . -j 72 {target_str} \n")
    p.run("cd ../script \n")
    p.commit()
    return p

def rebuild_async(targets: 'list[str]', silent: 'bool'):
    cmake_flags = []
    p = build_async(cmake_flags, targets, False)
    return p

@report_time_decorator
@report_compile_info_decorator
def rebuild(targets: 'list[str]'):
    p = rebuild_async(targets, False)
    p.wait()

@report_time_decorator
@report_compile_info_decorator
def build(cmake_flags: 'list[str]', targets : 'list[str]', silent: 'bool'):
    p = build_async(cmake_flags, targets, silent)
    p.wait()


def build_debug(targets: 'list[str]', silent: 'bool'):
    cmake_flags = ["-DCMAKE_BUILD_TYPE=Debug"]
    return build(cmake_flags, targets, silent)


def build_release(targets: 'list[str]', silent):
    cmake_flags = ["-DCMAKE_BUILD_TYPE=Release"]
    return build(cmake_flags, targets, silent)


def chunk(lst, n):
    """Yield successive n-sized chunks from lst."""
    for i in range(0, len(lst), n):
        yield lst[i:i + n]

def listdir_recursive(dir, pattern, full_path=True):
    matches = []

    for dirpath, dirnames, filenames in os.walk(dir):
        for filename in fnmatch.filter(filenames, pattern):
            if full_path:
                matches.append(os.path.join(dirpath, filename))
            else:
                matches.append(filename)

    return matches


def listdir(directory, pattern='*', full_path=False):
    """
    List all files in a given directory that match the given pattern.

    :param directory: The directory to search within.
    :param pattern: The search pattern, e.g., '*.txt' for all text files.
    :param full_path: Boolean indicating whether to return full path or just filenames.
    :return: A list of matching file names or file paths.
    """
    # Ensure the directory exists
    if not os.path.exists(directory):
        return []

    # List all files that match the given pattern
    matching_files = [f for f in os.listdir(directory) if fnmatch.fnmatch(f, pattern) and os.path.isfile(os.path.join(directory, f))]

    # If full_path is True, return the full path; otherwise, return the filename
    return [os.path.join(directory, f) if full_path else f for f in matching_files]



def is_valid_ip(address: 'str'):
    parts = address.split(".")
    if len(parts) != 4:
        return False
    for item in parts:
        if not 0 <= int(item) <= 255:
            return False
    return True


def is_valid_port(port: 'str|int'):
    port = int(port)
    return port > 1 and port < 65535


def banner(msg):
    return print(f"{bcolors.HEADER}================= {msg} ================={bcolors.ENDC}")

def debug(msg):
    if config.DEBUG_SCRIPT:
        return print(f"{msg}")

def state(msg):
    return print(f"{bcolors.BOLD}{bcolors.OKGREEN}{msg}{bcolors.ENDC}")


def error(msg):
    return print(f"{bcolors.FAIL}{msg}{bcolors.ENDC}")


def warning(msg):
    return print(f"{bcolors.WARNING}{msg}{bcolors.ENDC}")


def query_cmake(field: 'str'):
    cmake_cache_file = os.path.join(config.BUILD_DIR, "CMakeCache.txt")
    try:
        with open(cmake_cache_file) as f:
            for line in f:
                if field in line:
                    name, others = line.strip().split(":")
                    t, value = others.strip().split("=")
                    return value
    except Exception as e:
        error(f"Failed to query cmake for {field}: {e}")


def query_cmake_build_type():
    return query_cmake("CMAKE_BUILD_TYPE")


def query_build_flags(flag: 'str') -> 'bool':
    build_cmd_path = os.path.join(config.BUILD_DIR, "compile_commands.json")
    try:
        with open(build_cmd_path) as file:
            build_cmd = json.load(file)
        first_cu = build_cmd[0]
        commands = first_cu["command"]
        return flag in commands
    except Exception as e:
        error(f"Failed to query build_flags {flag}: {e}")



def query_asan_enabled():
    return query_build_flags("-fsanitize")


def report_compile_info():
    build_type = query_cmake_build_type()
    if build_type != "Release":
        warning(f"Build type: {build_type}")
    else:
        state(f"Build type: {build_type}")
    asan_enabled = query_asan_enabled()
    if asan_enabled:
        warning(f"Asan enabled: True")
    else:
        state(f"Asan enabled: False")
    no_omit_frame_ptr = query_build_flags("-fno-omit-frame-pointer")
    if no_omit_frame_ptr:
        warning(f"Frame pointer: no omit")
    ndebug = query_build_flags("-DNDEBUG")
    if not ndebug:
        warning(f"NDEBUG: not defined")




