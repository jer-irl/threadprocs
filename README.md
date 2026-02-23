# uspacelab - cursed alternative to both threads AND processes

This README was written entirely by a human.  Unless otherwise specified, all errors are intentional to give a sense of "real person" hospitality `:)`.  Claude assisted with some code for this proof-of-concept science experiment, particularly the ELF loading.

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

- [x] _Rough_ [proof of concept example](./example/dummy_prog1.sh)
- [ ] Production quality
- [ ] Secure
- [ ] Documentation

## Design

### Launcher + server in user space



#### File descriptors, env, etc

#### Alternative

- Driver
- mpi-style launcher

### Launching programs

#### Alternatives

- pthreads-based
- Full dynamic loader
- dlmopen?

### Waved hands

- Signals
- Resource leaks on exit
- Security
- Interaction with setuid bit?

## Comparisons

### OpenMP

### MPI

### Multithreaded actor model

### Google Serviceweaver / https://dl.acm.org/doi/10.1145/3593856.3595909

## Lessons Learned

- pthreads nptl impl, quite architecture specific.
- ARM register points to pthread_t
- TLS impl
- Default position independence
- 


