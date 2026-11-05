#ifndef SLIM_MEM_H
#define SLIM_MEM_H
#include <sys/types.h>

/* Memory of a process (or a tree), in kB.
 * pss/rss/uss: classic numbers (uss = Private_Clean + Private_Dirty).
 * anon/shmem/file: the Pss_Anon / Pss_Shmem / Pss_File split of pss.
 * file_union: resident file-backed pages mapped by the tree, each file page
 *   counted ONCE however many processes map it (from /proc/PID/maps +
 *   /proc/PID/pagemap "present" bits; unprivileged). Unlike Pss_File it does
 *   not shrink when unrelated processes (other Chromiums) map the same binary.
 * iso = anon + shmem + file_union: the "isolated footprint", what the tree
 *   would occupy alone on the machine. slimium's official RAM metric.
 * swap: SwapPss (not part of iso). */
typedef struct {
    int procs;
    long pss_kb, rss_kb, uss_kb;
    long anon_kb, shmem_kb, file_kb, file_union_kb, iso_kb, swap_kb;
} slim_mem;

/* Sum memory of pid and all its descendants (from /proc). Returns 0 on success,
 * -1 if root does not exist. Processes that exit mid-scan are skipped; a process
 * whose smaps_rollup cannot be read (EACCES) is counted with its statm RSS only. */
int slim_mem_tree(pid_t root, slim_mem *out);

/* Memory of one process (procs = 1; file_union = its own resident file pages).
 * Returns 0 on success, -1 if it is gone. */
int slim_mem_proc(pid_t pid, slim_mem *out);

/* Like slim_mem_tree, and calls cb(pid, its own numbers, ctx) for every process
 * of the tree (root first). cb may be NULL. */
int slim_mem_tree_each(pid_t root, void (*cb)(pid_t pid, const slim_mem *m, void *ctx),
                       void *ctx, slim_mem *out);

/* Several roots summed as one set (file pages shared between the trees are
 * counted once). Returns 0 if at least one root exists. */
int slim_mem_trees(const pid_t *roots, int n, void (*cb)(pid_t pid, const slim_mem *m, void *ctx),
                   void *ctx, slim_mem *out);

#endif
