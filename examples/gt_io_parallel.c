/* Example: parallel write / read / modify over io_uring on green threads.
 *
 * Spawns many green threads across several worker OS threads (real multi-core
 * M:N scheduling). Each green thread, using blocking-style dlsm-io calls that
 * actually submit to io_uring and park the green thread until completion:
 *
 *     WRITE  value=id*100  ->  fsync
 *     READ   verify == id*100
 *     MODIFY value += 7 (read-modify-write)  ->  fsync
 *     READ   verify == id*100 + 7
 *
 * While one green thread waits on I/O, its worker runs others, so the whole
 * batch overlaps. A high-water mark of in-flight tasks shows the concurrency.
 *
 * Usage: gt_io_parallel [nworkers=4] [ntasks=64]
 */
#define _GNU_SOURCE
#include "dlsm/io.h"
#include "dlsm/greenthread.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static _Atomic int g_inflight;
static _Atomic int g_max_inflight;
static _Atomic int g_ok;

typedef struct { dlsm_io *io; int id; } task_ctx;

static void note_peak(int cur) {
    int prev = atomic_load_explicit(&g_max_inflight, memory_order_relaxed);
    while (cur > prev &&
           !atomic_compare_exchange_weak_explicit(&g_max_inflight, &prev, cur,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) { }
}

static int temp_fd(void) {
    char path[] = "/tmp/dlsm_ex_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) { unlink(path); }
    return fd;
}

static void task_fn(void *arg) {
    task_ctx *t = (task_ctx *)arg;
    int cur = atomic_fetch_add_explicit(&g_inflight, 1, memory_order_relaxed) + 1;
    note_peak(cur);

    int fd = temp_fd();
    int rc = 0;
    int32_t value = t->id * 100;

    /* WRITE */
    if (dlsm_io_write_at(t->io, fd, &value, sizeof value, 0) != (ssize_t)sizeof value) { rc = 1; }
    if (!rc && dlsm_io_fsync(t->io, fd) != 0) { rc = 2; }

    /* READ + verify */
    int32_t got = 0;
    if (!rc && dlsm_io_read_at(t->io, fd, &got, sizeof got, 0) != (ssize_t)sizeof got) { rc = 3; }
    if (!rc && got != t->id * 100) { rc = 4; }

    /* MODIFY: read-modify-write */
    int32_t modified = got + 7;
    if (!rc && dlsm_io_write_at(t->io, fd, &modified, sizeof modified, 0) != (ssize_t)sizeof modified) { rc = 5; }
    if (!rc && dlsm_io_fsync(t->io, fd) != 0) { rc = 6; }

    /* READ back + verify modification */
    int32_t final = 0;
    if (!rc && dlsm_io_read_at(t->io, fd, &final, sizeof final, 0) != (ssize_t)sizeof final) { rc = 7; }
    if (!rc && final != t->id * 100 + 7) { rc = 8; }

    if (fd >= 0) { close(fd); }

    int peak = atomic_load_explicit(&g_max_inflight, memory_order_relaxed);
    char line[160];
    int n = snprintf(line, sizeof line,
                     "[task %3d] write=%-6d read=%-6d modify->%-6d  %s  (peak in-flight=%d)\n",
                     t->id, value, got, final, rc == 0 ? "OK " : "FAIL", peak);
    (void)!write(STDOUT_FILENO, line, (size_t)n);

    if (rc == 0) { atomic_fetch_add_explicit(&g_ok, 1, memory_order_relaxed); }
    atomic_fetch_sub_explicit(&g_inflight, 1, memory_order_relaxed);
}

int main(int argc, char **argv) {
    int nworkers = (argc > 1) ? atoi(argv[1]) : 4;
    int ntasks   = (argc > 2) ? atoi(argv[2]) : 64;
    if (nworkers <= 0) { nworkers = 4; }
    if (ntasks <= 0)   { ntasks = 64; }

    printf("== dlsm parallel write/read/modify over io_uring ==\n");
    printf("workers=%d  tasks=%d\n\n", nworkers, ntasks);
    fflush(stdout); /* flush banner before the unbuffered per-task lines */

    dlsm_io *io = dlsm_io_new(256);
    if (!io) { fprintf(stderr, "dlsm_io_new failed (io_uring unavailable?)\n"); return 2; }
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(nworkers, 0);
    if (!rt) { fprintf(stderr, "runtime_new failed\n"); dlsm_io_free(io); return 2; }

    task_ctx *ctx = calloc((size_t)ntasks, sizeof *ctx);
    for (int i = 0; i < ntasks; i++) {
        ctx[i].io = io;
        ctx[i].id = i;
        dlsm_gt_spawn(rt, task_fn, &ctx[i]);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    dlsm_gt_run(rt);                 /* blocks until all green threads finish */
    clock_gettime(CLOCK_MONOTONIC, &t1);

    dlsm_gt_runtime_free(rt);
    dlsm_io_free(io);
    free(ctx);

    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    int ok = atomic_load(&g_ok);
    int peak = atomic_load(&g_max_inflight);
    printf("\n-- summary --\n");
    printf("ok=%d/%d  peak in-flight=%d  workers=%d  elapsed=%.2f ms\n",
           ok, ntasks, peak, nworkers, ms);

    /* success requires all correct AND demonstrable overlap (>1 in flight) */
    if (ok != ntasks) { printf("RESULT: FAIL (some tasks incorrect)\n"); return 1; }
    if (peak < 2)     { printf("RESULT: FAIL (no overlap observed)\n"); return 1; }
    printf("RESULT: PASS (all correct, ran concurrently)\n");
    return 0;
}
