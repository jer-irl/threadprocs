# Conclusions

I learned some fun lessons along the way:

- Claude Code is good at doing arcane low-level work more quickly than a human.
- pthread_t is a pointer to some interesting state.  In Linux and NPTL:
	- pthread_t is a pointer to per-thread data
	- On aarch64 (and I think x86_64) this pointer is also populated into a well-known architectural register by the kernel when execution context switches.  This way a thread can always read this register to access the pthread_t pointer value.
	- The pointed-to data includes a reference to thread-local storage.
	- The pointed-to data also includes a table with one record for each dynamically loaded library, so there can be thread-local storage for each library, even those loaded late.
	- Except for populating the architectural register, this is all implemented in glibc+NPTL (or equivalents).
- On Debian, GCC compiles position-independent binaries and libraries by default
	- Shared libraries pretty much always need position-independence
	- Executables don't have much runtime cost because branching by offset is inexpensive on aarch64 and x86_64.

pthreads are a choice, not a law of nature.
We can reach beyond them entirely from user space.
