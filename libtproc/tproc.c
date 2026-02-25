#include "tproc.h"

#include <elf.h>
#include <stddef.h>

extern char **environ;

static void *cached_registry;

/*
 * Runs before main().  Walks past environ's NULL terminator to
 * reach the raw auxv on the stack, then scans for our custom
 * AT_TPROC_REGISTRY entry.
 *
 * This must run before any setenv/putenv, which could relocate
 * the environ array away from the original stack.  Constructor
 * priority is left at the default (runs before main, after libc
 * init).
 *
 * We cannot use getauxval() because glibc discards AT_ types it
 * does not recognise.  We cannot use /proc/self/auxv because it
 * reflects the server process's execve auxv, not our synthetic
 * stack.
 */
__attribute__((constructor))
static void tproc_init(void) {
	if (!environ)
		return;

	char **p = environ;
	while (*p)
		p++;
	p++; /* skip NULL sentinel after envp */

	Elf64_auxv_t *auxv = (Elf64_auxv_t *)p;
	for (; auxv->a_type != AT_NULL; auxv++) {
		if (auxv->a_type == AT_TPROC_REGISTRY) {
			cached_registry = (void *)auxv->a_un.a_val;
			return;
		}
	}
}

void *tproc_registry(void) {
	return cached_registry;
}

int tproc_is_threadproc(void) {
	return cached_registry != NULL;
}
