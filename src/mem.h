#ifndef SLIM_MEM_H
#define SLIM_MEM_H
#include <sys/types.h>

typedef struct { int procs; long pss_kb; long rss_kb; long uss_kb; } slim_mem;

/* Sum memory of pid and all its descendants (from /proc). Returns 0 on success,
 * -1 if root does not exist. Processes that exit mid-scan are skipped; a process
 * whose smaps_rollup cannot be read (EACCES) is counted with its statm RSS only. */
int slim_mem_tree(pid_t root, slim_mem *out);

/* Memory of one process (procs = 1). Returns 0 on success, -1 if it is gone. */
int slim_mem_proc(pid_t pid, slim_mem *out);

/* Like slim_mem_tree, and calls cb(pid, its own numbers, ctx) for every process
 * of the tree (root first). cb may be NULL. */
int slim_mem_tree_each(pid_t root, void (*cb)(pid_t pid, const slim_mem *m, void *ctx),
                       void *ctx, slim_mem *out);

#endif
