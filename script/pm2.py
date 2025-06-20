#!/usr/bin/python3
import asyncio

DEBUG = False

class Process:
    def __init__(self, name: 'str', init_cmds: 'list[str]', silent=False):
        """
        @name [str] an arbitrary name of the process
        @init_cmd [str] a sequence of command to launch the process. See @subprocess.Popen
        @silent [bool] whether or not print output
        """
        self.__name = name
        self.__init_cmds = init_cmds
        self.__silent = silent
        self.__output_handlers = []
        if DEBUG:
            print(f"[debug] Process created. name: {name}, init_cmds: {init_cmds}, silent: {silent}")
    
    async def run(self, cmd: 'str'):
        cmd += " \n"
        self.__process.stdin.write(cmd.encode('utf-8'))
        await self.__process.stdin.drain()
    
    def register_output_handler(self, fn):
        self.__output_handlers.append(fn)

    async def launch(self):
        self.__process = await asyncio.create_subprocess_exec(
            *self.__init_cmds,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
    
    async def read_line(self):
        line = await self.__process.stdout.readline()
        if not line:
            return None
        line = line.decode('utf-8').rstrip()
        if not self.__silent:
            print(f"[{self.__name}] {line}")
        
        for fn in self.__output_handlers:
            await fn(self, line)
        return line
    
    async def poll(self):
        while self.__bool__():
            line = await self.__process.stdout.readline()
            if not line:
                return self.returncode()

            line = line.decode('utf-8').rstrip()
            if not self.__silent:
                print(f"[{self.__name}] {line}")
            
            for fn in self.__output_handlers:
                await fn(self, line)

        return self.returncode()

    async def wait(self):
        self.__process.stdin.close()
        await self.poll()
        await self.__process.wait()
    
    async def kill(self):
        self.__process.kill()
        await self.__process.wait()
    
    def returncode(self):
        return self.__process.returncode
    
    def stdout_at_eof(self):
        return self.__process.stdout.at_eof()
    
    def __bool__(self):
        return self.returncode() is None or not self.stdout_at_eof()
    
    
async def create_process(name, init_cmds, silent=False):
    p = Process(name, init_cmds, silent)
    await p.launch()
    return p

async def create_remote_process(name, ip='127.0.0.1', user=None, init_cmds=None, silent=False):
    if user is None:
        user_str = ip
    else:
        user_str = f"{user}@{ip}"
    _init_cmds = ['ssh', '-tt', '-o LOGLEVEL=QUIET', user_str]
    if init_cmds is not None:
        _init_cmds = _init_cmds + init_cmds
    p = await create_process(name, _init_cmds, silent)
    return p

async def local_main():
    silent=False
    p = await create_process("local", ["ls", "-l"], silent=silent)

    await p.wait()
    # Or equivalently:
    # while p:
    #     print(await p.read_line())

    print(f"Retcode: {p.returncode()}")

async def remote_main():
    p = await create_remote_process('ssh', '10.0.2.132')

    await p.run("cd /home/yanbin/ford/script")
    await p.run("g++ echo.cpp")
    await p.run("./a.out")
    
    count = 0
    while True:
        line = await p.read_line()

        if "Done." in line:
            count += 1
            if count < 100:
                await p.run("100")
            else:
                print("Kill.")
                await p.kill()
                break

    await p.wait()

    print(f"Retcode: {p.returncode()}")

async def remote_err_main():
    p = await create_remote_process('ssh', '10.0.2.132')

    await p.run("cd /home/yanbin/ford")
    await p.run("./not_exist.out")
    await p.run("exit")
    
    count = 0
    while p:
        line = await p.read_line()

        if "Done." in line:
            count += 1
            if count < 100:
                await p.run("100")
            else:
                print("Kill.")
                await p.kill()
                break

    await p.wait()

    print(f"Retcode: {p.returncode()}")

async def remote_simple_main():
    p = await create_remote_process('ssh', 'localhost')

    await p.run("ls -l")
    await p.run("exit")
    
    await p.wait()

    print(f"Retcode: {p.returncode()}")

async def main():
    # tasks = [asyncio.create_task(local_main()), asyncio.create_task(remote_main())]
    # tasks = [asyncio.create_task(remote_err_main())]
    tasks = [asyncio.create_task(remote_main())]
    # tasks = [asyncio.create_task(remote_simple_main())]
    # tasks = [asyncio.create_task(local_main())]
    # tasks = [asyncio.create_task(local_main()), asyncio.create_task(local_main()), asyncio.create_task(local_main())]

    # Wait for all subprocesses to finish
    await asyncio.gather(*tasks)


if __name__ == '__main__':
    asyncio.run(main())