#ifndef SLIM_MEM_H
#define SLIM_MEM_H
#include <sys/types.h>

typedef struct { int procs; long pss_kb; long rss_kb; long uss_kb; } slim_mem;

/* Sum memory of pid and all its descendants (from /proc). Returns 0 on success. */
int slim_mem_tree(pid_t root, slim_mem *out);

#endif
