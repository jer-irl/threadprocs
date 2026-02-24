# threadprocs

This repository contains experimental code for thread-like processes, which blend the Posix process model with the pthread programming model.

All Markdown files were written by hand.
Claude assisted with some code for this proof-of-concept, particularly around the ELF loading and the aarch64 trampoline.

DO NOT use this for anything beyond a trivial test.  Bad things will probably happen.

## Status

- [x] _Rough_ [proof of concept example](./example/dummy_prog1.sh), aarch64+Linux only! (Developed in VM on M1 Macbook Air)
- [ ] Architecture agnostic
- [ ] Production quality
- [ ] Secure
- [ ] Documentation
- [ ] Tooling for peer/service discovery
- [ ] Safe Rust example

## Getting Started

Use Linux on aarch64, other architectures not supported.
This was developed in a VM running Debian on a Macbook Air M1.

Dependencies:

```sh
apt install build-essential liburing-dev
```

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

## Limitations

The main (known) requirement is that target applications be compiled as "position independent" executables, with all loadable executables also being position independent.
This is the default with Debian GCC, and used to support various flavors of ASLR.
This also doesn't carry much overhead over fixed-position executables, and the costs of position independed dynamic shared libraries are well understood and easily mitigated with some effort.

Another major restriction is that because `brk()` and `sbrk()` are address space-global from a kernel perspective, applications can't reliably use them.
The server sets the `MALLOC_MMAP_THRESHOLD_=0` environment variable to change the default glibc behavior to avoid these.
Things will break if you try to use this.

Another restriction is of using `mmap` with `MAP_FIXED` in unfriendly ways.
This is always advised against regardless (see the manpage section [Using MAP_FIXED safely](https://man7.org/linux/man-pages/man2/mmap.2.html)).
Perhaps a future extension would be to hook `mmap` and related syscalls in the `server` process to enforce applications don't clobber each other.
The conventional `~MAP_FIXED` use of `mmap()` won't cause issues.
