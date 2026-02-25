#include <tproc.h>
#include <stdio.h>
#include <string.h>

int main(void) {
	if (tproc_is_threadproc()) {
		void *reg = tproc_registry();
		printf("threadproc: yes\n");
		printf("registry: %p\n", reg);

		const char *marker = "detect was here";
		memcpy(reg, marker, strlen(marker) + 1);
		printf("wrote: %s\n", (const char *)reg);
	} else {
		printf("threadproc: no\n");
	}
	return 0;
}
