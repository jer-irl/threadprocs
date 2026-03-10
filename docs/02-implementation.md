# Design

The utility is distributed as 2 executable programs
The first is used to serve as a shared address space, and advertises itself as a Unix socket:

```sh
$ ./server /path/to/my.sock
# runs until killed
```

The second is a launcher which can launch and "attach" a program to the any running server:

```sh
$ ./launcher /path/to/my.sock /path/to/my_program my_program_arg1 my_program_arg2
```

Any number of processes can attach to a running server.
This is inherently very insecure, and I also haven't implemented authorization in the socket connection, though SCM_CREDENTIALS would be a decent starting place.
If you have a running server, Mallory could currently trivially run untrusted code as long as she can access the socket, and could cause any number of problems with existing running code by attaching and interfering.

To ease the server code implementation, the main poll loop uses io_uring (through liburing).

## Launcher illusion

The attached program is never actually launched (directly) by the launcher program.
Instead, the launcher will connect to the server socket, and then request that the server launch the program itself.
To make this look somewhat natural, the launcher will share information about its execution environment and its target application, and set up some forwarding/sharing mechanisms.

### Execution environment

The launcher process will communicate the following information to the server so the server may launch a threadproc "as if" it was a subprocess of the launcher process.

- Target binary path
- Command line arguments
- Environment variables
- Current working directory

The server will set up the threadproc entry stack and filesystem environment to match the provided information.

There are probably many additional values that are needed to properly sustain the illusion, particularly aroud `ptrace` and signals, but also others like floating point environment, cgroups, namespaces, etc.

### stdin/out/err file descriptors

Greatly helping with the illusion, the launcher also shares its stdin/stdout/stderr file descriptors with the server through an `SCM_RIGHTS` auxilliary data Unix socket message.
When the target program is "launched" in the server, the `dup3` syscall is used to replace the "child" stdxxx file descriptors with those from the launcher process.

The effect is that when launching a process with the launcher, it appears as if the launcher console output, input, etc are all working as if our launcher were actually running the program.

### Signal forwarding and exit notification

When the launcher process receives a signal Ctrl-C, `kill`, or some other mechanism, its signal handler will forward the signal to the actual running threadproc using one of the following methods.

- If the threadproc is running and the launcher has been notified of its PID, the launcher process will forward the signal by using `kill()` itself.
- If the launcher doesn't yet know the threadproc's PID, it will send a message through the server socket, and the server will use `kill()` to forward a signal.

When the "target process" exits, the server sends a notification to the launcher, including exit code.
The launcher exits with this code, completing the farce.

### Launcher app alternatives

I've been doing more Linux device driver work in the past year, and my first intinct was to approach this project with in-kernel code.
I'm not sure the best approach, and given `ld-linux.so` and `libc` are all in userspace, I think user space is the best way to go.
Loading drivers also would require escalated privileges, and generally be more painful to work with.

A slightly different approach could be to merge the server and launcher, where the first `launcher` to execute creates the socket and functions as the server.
I am not sure how well this holds up when the server executes earlier than launched applications, but it might be interesting to explore.

## Launching threadprocs

Once the server process knows the entire description of a new program to launch, it uses the `clone3` and `dup3` system calls in tricky ways to accomplish a hybrid process/thread.
In Linux (the OS Kernel), there is not really a concept of a "thread," only of processes and peer "process groups." which share varying resources.
NPTL (Native Posix Threading Library) is a subcomponent of glibc which implements Posix process + thread semantics on top of the Linux system calls.
The [manpage](https://man7.org/linux/man-pages/man7/nptl.7.html) describes some interesting workarounds for edges where Posix semantics don't map nicely onto Linux system calls.

There are several parts of the Posix/libc worldview that resist the "process as threads" approach, including several conventionally "process global" concepts:

- Signal and signal handlers
- `main()` entry point
- Global variables
	- Per-thread global variables
	- Per-dynamic shared library global variables
	- Per-dynamic shared library, per-thread global variables
- File descriptors, seek position offsets, etc.
- Userspace + Posix environmental:
	- Environment variables
	- Command line arguments
- Kernel Posix-driven environmental:
	- Thread name
	- FP env, 
	- Others set with `prctl` and `arch_prctl`
- cgroups, namespaces, floating point execution config, etc.
- Memory mappings, brk()

Thankfully, the Linux system calls offer a lot of power to massage the system into the hybrid model we are seeking, and we can fake the userspace-only concepts easily enough.

### File descriptor cleanup

To avoid leakage of file descriptors between different threadprocs, the trampoline code also uses `close_range` to ensure only these three standard file descriptors remain.

### Process OS info

The executing threadproc uses `prctl` to change its "comm" value, and also launches in its own PID (not in a thread group).

### Launch sequence

Once the server is ready to launch a process, the following sequence occurs

1. The server process opens the target binary file and parses its ELF headers
2. Using the ELF header information, it `mmaps` necessary regions into the server address space, and also creates some anonymous regions as needed.  `mmap()` with no `MAP_FIXED` ensures these mappings don't collide with those from other processes, as long as they are also not using the flag unsafely.
3. If the target ELF binary specifies an "interpreter" (typically ld-linux.so), the interpreter is also loaded in the same way.  Note that existing mappings of the same interpreter are not uses, and each target gets an independent mapping.
4. A "top-level" stack is set up for a new instance of the interpreter program (or if not present, `_start`)
	- Includes the argv and environ arrays
		- The `MALLOC_MMAP_THRESHOLD_=0` variable is added to prevent glibc from using `brk()`
	- Entry point for target binary
5. A "trampoline stack" is set up for the `clone3` system call, but nothing important really needs to go in here except for function arguments.
6. `clone3()` args are prepared with the following notable pieces
	- Trampoline stack
	- No TLS
	- Notable included flags:
		- `CLONE_VM` for a single virtual address space, unlike `exec()`
		- `CLONE_CLEAR_SIGHAND`
	- Notable exluded flags:
		- `CLONE_FILES` so file descriptors are not "globally shared" between the child and server
		- `CLONE_FS` so cwd is not shared
		- `CLONE_PARENT` so the spawned thread's parent is the server, and the child is not a peer process with the server
		- `CLONE_SIGHAND` so handlers are not shared
		- `CLONE_THREAD` to avoid making the child look like only a thread
7. The `extern "C"` trampoline function is called.  This is implemented in architecture-specific assembly (aarch64 and x86_64).
8. The `clone3()` system call is invoked, and the return value is checked, as with `fork()`.  If non-zero, the trampoline returns up to the server poll loop.
9. If in the child "thread" of execution, the `dup3()` system call is used to move the launcher `stdin/out/err` file descriptors into the conventional `0/1/2` values.  Once these are installed, the `close_range()` syscall is used to close all other open file descriptors, and avoid leaking any between launched processes.
10. The new threadproc uses `chdir()` to change its working directory to match the launcher process
11. The new threadproc uses `prctl()` to change its entry in `/proc/[pid]/comm`.
12. The trampoline bounces into the entry point, which will typically be in `ld-linux.so`.  This program will do a lot of things, but resources other than virtual memory should be isolated.

This is overall similar in some ways to what pthreads implementations must do, but because we bounce to the ELF binary's entry point, we don't have to worry about playing nicely with libpthread's supporting data structures.
Also note that the trampoline and entry point don't get to use libc, and must program directly against the Linux system calls.
We expect that the target program entry point will initialize its own instance of libc and the associated "global" data structures.

### Threadproc launch alternatives

I initially avoided using bouncing to the target binary's interpreter, and instead tried to load all dynamic objects directly.
The recursive nature of this grew to be painful, and it was also painful to not be able to use libc in the newly spawned threadproc when that child was responsible for implementing more initialization.

I also explored spawning child threads as simple pthreads, and then using `unshare()` to seperate out resources that should be isolated.
Using pthreads wasn't the benefit I'd hoped, and got messy when I'd try to trampoline away and leave the pthread resources dangling.

Similarly, I also explored using `dlmopen` to provide isolation between targets by leveraging libc functionality.
It quickly became clear that it was best to have multiple independent instances of libc instead.

## Hand-waves and next steps

- [ ] Test signals and unclean exits
- [ ] Resource leaks on exit
	- Hook `mmap()`?
- [ ] Socket credentials
- [ ] Memory access, maybe something with `userfaultfd()`
- [ ] Floating point execution environment
- [ ] Interaction with setuid bit?  I'm not sure if this is only in `exec()` or if there's a way to recreate it without additional privilege.  Probably not.
- [ ] Ptrace and debugging of targets?
