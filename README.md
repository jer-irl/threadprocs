# threadprocs

This repository contains experimental code for thread-like processes, or __multiple programs running in a shared address space__.
This blends the Posix process model with the Posix multi-threading programming model.

All Markdown files were written by hand.
Claude assisted with some code for this proof-of-concept, particularly around the ELF loading and the aarch64 trampoline.

DO NOT use this for anything beyond a trivial test.  Bad things will probably happen.

## Elevator pitch

The `server` utility "hosts" a virtual address space, and by using `launcher` to start programs, those launched programs can coexist in the hosted address space.
Applications can share pointers in the virtual address space through some out-of-band mechanism ([Demo](#demo) uses copy/paste, dummy_server/client uses sockets, libtproc provides server-global scratch space), and then _directly dereference those pointers_, as they're valid in the shared address space.

## Demo

The code for the demoed programs is at `example/allocstr.cpp` and `example/printstr.cpp`, and neither contains any magic (`/proc/[pid]/mem`, etc), nor awareness of the server and launcher.

https://github.com/user-attachments/assets/496b68fb-3965-4c44-874f-a96d370c92cb

## libtproc

`libtproc` provides very rudimentary detection of execution as a threadproc, and allows hosted threadprocs to access a "server-global" scratch space.
Applications can build tooling using this space to implement service discovery and bootstrap shared memory-backed IPC.

## Status

- [x] _Rough_ proof of concept examples in `test/`, aarch64+Linux only! (Developed in VM on M1 Macbook Air)
- [ ] Architecture agnostic
- [ ] Production quality
- [ ] Secure
- [ ] Documentation
- [x] Tooling for peer/service discovery (basic)
- [ ] Safe Rust example

## Getting started

Use Linux on aarch64, other architectures not supported.
This was developed in a VM running Debian on a Macbook Air M1.

Dependencies:

```sh
apt install build-essential liburing-dev
git submodule update --init
```

Notably there is no dependency no ELF libraries aside from Linux system headers, though those would probably make the code nicer.

Building:

```sh
make
```

Run auto integration tests:

```sh
make test
```

Or run your own programs in a shared address space:

```sh
./buildout/server /tmp/mytest.sock &
./buildout/launcher /tmp/mytest.sock program1 arg1 arg2 &
./buildout/launcher /tmp/mytest.sock program2 arg3 arg4
```

Read the [overview](./docs/01-overview.md) or [implementation](./docs/02-implementation.md) for information on the project, or read [comparisons to existing work](./docs/03-comparisons.md).
I've also collected some fun lessons learned in [conclusions](./docs/04-conclusions.md).

## Technical limitations

- Target applications must be compiled as "position independent code," as do any dynamically loaded objects.
	- This is standard for dynamically linked libraries, and default for executable binaries compiled in many modern distros in order to support flavors of ASLR.
	- Properly architected libraries can mitigate most drawbacks of this, and executable files also carry minimal overhead.
- `brk()` (and `sbrk()`) cannot be used reliably, because they are "address space global" to the kernel, and processes typically assume they won't be called from unexpected places.
	- The server sets the `MALLOC_MMAP_THRESHOLD_=0` environment variable for children to avoid the default glibc behavior and avoid these calls.
- `mmap` with `MAP_FIXED` can't be used without first "reserving" a non-fixed mapping.
	- This is generally true of any program, and "unreserved" `MAP_FIXED` use is unsafe even in standard Linux programs.
	- See the manpage section [Using MAP_FIXED safely](https://man7.org/linux/man-pages/man2/mmap.2.html)
- Debugging and `ptrace()` are not supported.
	- It may be possible to add partial support, but I suspect GDB makes some assumptions that would be difficult to satisfy

There are other less pertinent limitations around the edges.
For example, threadprocs have `/proc/[pid]/comm` values which reflect their launched binary, but `cmdline` isn't settable.
`exec()` syscalls also "escape" the threadproc scheme, which is probably desired may cause subtle issues.

## Practical limitations

My initial vision was for threadprocs to pass `std::unique_ptr`s to each other, and support IPC with nested data.
ABI aside, the major hiccup is that even if threadproc 1 releases a pointer, and threadproc 2 wraps it in a `std::unique_ptr`, when the destructor is called and it comes time to de-allocate the memory, threadproc 2 won't be able to do so.

Having independent libc, libstdc++, and rust libstd instances for each tproc greatly reduces the technical dependencies on launched programs, but it also means that a threadproc cannot deallocate memory allocated by another threadproc.

One could architect their application around this limitation, and ensure memory is always handed back to the tproc which allocated it so it can be de-allocated correctly.
You could imagine building application frameworks to support this, but I consider them outside the scope of this project.
