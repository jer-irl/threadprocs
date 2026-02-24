# threadprocs

This README was written entirely by a human.  Claude assisted with some code for this proof-of-concept, particularly around the ELF loading and the aarch64 trampoline.

DO NOT use this for anything beyond a trivial test.  Bad things will probably happen.

## Context

Working in latency-sensitive trading, IO bandwidth-bound AI training, and messaging rate-bound HPC networking, I have a brainworm for IPC mechanisms.
I recently contributed to Cornelis Networks's Super NIC device driver, and had a lot of fun applying userspace IPC approaches to a kernel-space RDMA driver.

Conventional wisdom for single-host IPC within a single processor / NUMA / memory domain node is to use shared memory IPC.
There are a few prevailing approaches, but for most workloads with multiple threads, they typically rely on lock-free queues.

One approach is to design a "flat" message structure (typically expressed as flat C structs) which can be written into a simple shared memory ring.
If messages are small and self-contained this works well.
If messages are not self-contained, or are large, this can require some data copying.
This approach is possible in applications architected as a single multi-threaded process, and as applications architected as multiple processes interfacing through shared memory regions (fs-backed or perhaps shared through Unix sockets).

Another approach is to pass pointers in a queue, ideally with unique_ptr and lock discipline.
This works best in a language like Rust for safety, but with some effort it can work well in less safe languages.
In addition to safety concerns, false sharing is also a consideration.
This approach also relies on pointers being valid between threads, and so only possible in a single address space, most naturally (in Linux) accomplished in a single multi-threaded process.

Single multi-threaded processes carry some limitations and drawbacks.

- Tooling for thread affinity is somewhat less mature than tooling for process affinity, and often requires application self-awareness of affinity concerns.
- All threads in a process share a lifetime.
	- If one thread exits, all threads exit, which is good in some cases but certainly more frequent in larger applications.
	- All threads run from the same binary, and this code cannot be updated without restarting all threads together.  Build times also tend to get large, and releasing large binaries with many subcomponents can be a difficult process.

There are other limitations and drawbacks around a shared memory programming model generally, despite C and C++ memory model development, but I'll ignore those.

## Goal

POSIX has served as a remarkably resilient set of standard interfaces, with the process model and pthreads API widely adopted in many operating systems.
These abstractions have received criticism, but modern C and C++ continue to "lean in" to the programming model.

In performance-sensitive applications of significant complexity, I posit that basic pthreads is not globally optimal, and that there are improvements to be had.  With this project, I want to explore a possible different approach to developing and deploying these applications.

## Status

- [x] _Rough_ [proof of concept example](./example/dummy_prog1.sh), aarch64+Linux only! (Developed in VM on M1 Macbook Air)
- [ ] Architecture agnostic
- [ ] Production quality
- [ ] Secure
- [ ] Documentation
- [ ] Tooling for peer/service discovery
- [ ] Safe Rust example

## Design

### Target application requirements

The main (known) requirement is that applications be compiled as "position independent" executables, with all loadable executables also being position independent.
This is the default with Debian GCC, and used to support various flavors of ASLR.
This also doesn't carry much overhead over fixed-position executables, and the costs of position independed dynamic shared libraries are well understood..

Another restriction is of using `mmap` with `MAP_FIXED` in unfriendly ways.
This is always advised against regardless (see the manpage section [Using MAP_FIXED safely](https://man7.org/linux/man-pages/man2/mmap.2.html)).
Perhaps an extension would be to hook `mmap` and related syscalls in the `server` process to enforce applications don't clobber each other.
The conventional `~MAP_FIXED` approach won't cause issues.

### Launcher + server in user space

The utility is distributed as 2 executable programs
The first is used to serve as a shared address space, and advertises itself as a Unix socket:

```sh
$ buildout/server /path/to/my.sock
# runs until killed
```

The second is a launcher which can launch and "attach" a program to the any running server:

```sh
$ buildout/launcher /path/to/my.sock /path/to/my_program my_program_arg1 my_program_arg2
```

Any number of processes can attach to a running server.
This is inherently very insecure, and I also haven't implemented authorization in the socket connection, though SCM_CREDENTIALS would be a decent starting place.
If you have a running server, Mallory could currently trivially run untrusted code as long as she can access the socket, and could cause any number of problems with existing running code by attaching and interfering.

#### File descriptors, env, etc

The attached program is never actually launched (directly) by the launcher program.
Instead, the launcher will connect to the server socket, and then request that the server launch the program itself.
To make this look somewhat natural, the launcher will share information about its execution environment and its target application, including

- Target binary path
- Command line arguments
- Environment variables
- Current working directory

There are probably many additional values that are needed to properly sustain the illusion.
Greatly helping with the illusion is the fact that the launcher also shares its stdin/stdout/stderr file descriptors with the server through an `SCM_RIGHTS` auxilliary data Unix socket message.
When the target program is "launched" in the server, the `dup3` syscall is used to replace the "child" stdxxx file descriptors with those from the launcher process.

The effect is that when launching a process with the launcher, it appears as if the launcher console output, input, etc are all working as if our launcher were actually running the program.

When the "target process" exits, the server sends a notification to the launcher, including exit code.
The launcher exits with this code, completing the farce.

To ease the server code implementation, the main poll loop uses io_uring (through liburing).

#### Alternatives

I've been doing more Linux device driver work in the past year, and my first intinct was to approach this project with in-kernel code.
I'm not sure the best approach, and given `ld-linux.so` and `libc` are all in userspace, I think user space is the best way to go.
Loading drivers also would require escalated privileges, and generally be more painful to work with.

A slightly different approach could be to merge the server and launcher, where the first `launcher` to execute creates the socket and functions as the server.
I am not sure how well this holds up when the server executes earlier than launched applications, but it might be interesting to explore.

### Launching programs

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
- cgroups, namespaces, etc.

Thankfully, the Linux system calls offer a lot of power to massage the system into the hybrid model we are seeking, and we can fake the userspace-only concepts easily enough.

#### Approach

Once the server is ready to launch a process, the following sequence occurs

1. The server process opens the target binary file and parses its ELF headers
2. Using the ELF header information, it `mmaps` necessary regions into the server address space, and also creates some anonymous regions as needed.  `mmap()` with no `MAP_FIXED` ensures these mappings don't collide with those from other processes, as long as they are also not using the flag unsafely.
3. If the target ELF binary specifies an "interpreter" (typically ld-linux.so), the interpreter is also loaded in the same way.  Note that existing mappings of the same interpreter are not uses, and each target gets an independent mapping.
4. A "top-level" stack is set up for a new instance of the interpreter program (or if not present, `_start`)
	- Includes the argv and environ arrays
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
7. The `extern "C"` trampoline function is called.  This is currently only implemented in ARM assembly.
8. The `clone3()` system call is invoked, and the return value is checked, as with `fork()`.  If non-zero, the trampoline returns up to the server poll loop.
9. If in the child "thread" of execution, the `dup3()` system call is used to move the launcher `stdin/out/err` file descriptors into the conventional `0/1/2` values.  Once these are installed, the `close_range()` syscall is used to close all other open file descriptors, and avoid leaking any between launched processes.
10. The trampoline bounces into the entry point, which will typically be in `ld-linux.so`.  This program will do a lot of things, but resources other than virtual memory should be isolated, and as long as the launched program isn't doing `MAP_FIXED` `mmap()` calls unsafely, and isn't trying to scan its address space and touch things it shouldn't, this seems to work.

This is overall similar in some ways to what pthreads implementations must do, but because we bounce to the ELF binary's entry point, we don't have to worry about playing nicely with pthread's supporting data structures.
Also note that the trampoline and entry point don't get to use libc, and must program directly against the Linux system calls.
We expect that the target program entry point will initialize its own instance of libc and the associated "global" data structures.

#### Alternatives

- unshare
- pthreads-based
- Full dynamic loader
- dlmopen?

### Waved hands and next steps

- [ ] Test signals and unclean exits
- [ ] Resource leaks on exit
	- Hook `mmap()`?
- [ ] Socket credentials
- [ ] Memory access, maybe something with `userfaultfd()`
- [ ] Interaction with setuid bit?  I'm not sure if this is only in `exec()` or if there's a way to recreate it without additional privilege.  Probably not.

## Comparisons

### OpenMP

### MPI

### Multithreaded actor model

### Google Serviceweaver / https://dl.acm.org/doi/10.1145/3593856.3595909

## Lessons learned along the way

- pthreads nptl impl, quite architecture specific.
- ARM register points to pthread_t
- TLS impl
- Default position independence
