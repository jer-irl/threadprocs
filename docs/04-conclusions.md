# Conclusions

I learned some fun lessons along the way:

- Claude Code is good at doing arcane low-level work more quickly than a human, and is good at getting up to speed on a small project.  However, it tends to code inconsistently, and over time code files can get messy.
- pthread_t is a pointer to some interesting state.  In Linux and NPTL:
	- pthread_t is a pointer to per-thread data, NOT the value of the PID (a mistake I've made many times over the years).
	- On aarch64 (and I think x86_64) this pointer is also populated into a well-known register by the kernel when execution context switches.  This way a thread can always read this register to access the pthread_t pointer value.
	- The pointed-to data includes a reference to thread-local storage.
	- The pointed-to data also includes a table with one record for each dynamically loaded library, so there can be thread-local storage for each library, even those loaded late.
	- Except for populating the well-known register, this is all implemented in glibc+NPTL (or equivalents).
- On Debian, GCC compiles position-independent binaries and libraries by default
	- Shared libraries pretty much always need position-independence
	- Executables don't have much runtime cost because branching by offset is inexpensive on aarch64 and x86_64.
- glibc uses `brk()` in addition to `mmap()` for allocation.  This causes issues with multiple threadprocs colliding

pthreads are a choice, not a law of nature.
We can reach beyond them entirely from unprivileged user space.
