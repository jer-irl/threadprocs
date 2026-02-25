#ifndef TPROC_H
#define TPROC_H

/*
 * Custom auxiliary vector type for threadproc service discovery.
 * 0x5450 is in the OS-specific range, well above the highest
 * kernel-defined AT_ value (AT_MINSIGSTKSZ = 51).
 *
 * The value carried by this entry is a pointer to a shared
 * registry page, mmap'd once by the server.  Because children
 * share the server's mm_struct (CLONE_VM), all threadprocs can
 * read and write it.
 */
#define AT_TPROC_REGISTRY 0x5450

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns the registry page pointer, or NULL if not running
 * as a threadproc.  Safe to call from main() onward.
 */
void *tproc_registry(void);

/*
 * Returns non-zero if this process was launched by the
 * threadproc server.
 */
int tproc_is_threadproc(void);

#ifdef __cplusplus
}
#endif

#endif /* TPROC_H */
