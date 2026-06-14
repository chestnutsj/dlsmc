/* Example: database-style concurrent access to ONE shared file.
 *
 * Many green threads, running across several worker cores, each run a series of
 * transactions against a single shared file holding one 8-byte record (a row /
 * page counter). Each transaction is a read-modify-write protected by a
 * green-thread mutex:
 *
 *     LOCK  ->  READ record (io_uring)  ->  value+1  ->  WRITE (io_uring)
 *           ->  fdatasync  ->  UNLOCK
 *
 * The mutex is built on park/unpark, NOT a spin lock: a green thread that finds
 * the lock held *yields* (parks, giving its worker to other green threads) and
 * is *switched back* (unparked) when the holder releases. This is the safe
 * pattern when the critical section performs I/O (which itself parks) — a spin
 * lock would burn the worker and could deadlock the cooperative scheduler
 * (see bwtree-design.md §7.3).
 *
 * Correctness proof: with the lock, the final record == total transactions
 * (no lost updates). The count of contended acquisitions shows how many times a
 * green thread yielded on the lock and was later switched back.
 *
 * Usage: gt_io_db [nworkers=4] [ntasks=16] [txns_per_task=50]
 */
#define _GNU_SOURCE
#include "dlsm/io.h"
#include "dlsm/greenthread.h"
#include "dlsm/sync.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Bind dlsm_gt_mutex (from dlsm-sync) to the greenthread runtime via injected
 * suspend ops. park()/unpark() are exactly where a contended locker yields its
 * worker and is later switched back; we count parks to report contention.
 * No pthread_mutex anywhere — the mutex's internal queue lock is dlsm-sync's
 * own ticket spinlock.
 * ------------------------------------------------------------------------- */
static _Atomic long g_contended; /* # of lock acquires that had to park (yield) */

static void *gt_current(void) { return dlsm_gt_self(); }
static void  gt_park_counted(void) {
    atomic_fetch_add_explicit(&g_contended, 1, memory_order_relaxed);
    dlsm_gt_park();
}
static void  gt_unpark_one(void *h) { dlsm_gt_unpark((dlsm_gt_task *)h); }
static const dlsm_suspend_ops GT_OPS = { gt_current, gt_park_counted, gt_unpark_one };

/* ---------------------------------------------------------------------------
 * Shared "database": one file, one 8-byte record at offset 0.
 * ------------------------------------------------------------------------- */
typedef struct {
    dlsm_io      *io;
    dlsm_gt_mutex *mtx;
    int           fd;
    int           id;
    int           txns;
    int           ok;
} txn_ctx;

static void txn_task(void *arg) {
    txn_ctx *c = (txn_ctx *)arg;
    c->ok = 1;
    for (int i = 0; i < c->txns; i++) {
        dlsm_gt_mutex_lock(c->mtx);               /* serialize access to the record */

        int64_t v = 0;
        if (dlsm_io_read_at(c->io, c->fd, &v, sizeof v, 0) != (ssize_t)sizeof v) { c->ok = 0; dlsm_gt_mutex_unlock(c->mtx); return; }
        v += 1;                                   /* modify */
        if (dlsm_io_write_at(c->io, c->fd, &v, sizeof v, 0) != (ssize_t)sizeof v) { c->ok = 0; dlsm_gt_mutex_unlock(c->mtx); return; }
        if (dlsm_io_fdatasync(c->io, c->fd) != 0) { c->ok = 0; dlsm_gt_mutex_unlock(c->mtx); return; }

        dlsm_gt_mutex_unlock(c->mtx);
    }
}

int main(int argc, char **argv) {
    int nworkers = (argc > 1) ? atoi(argv[1]) : 4;
    int ntasks   = (argc > 2) ? atoi(argv[2]) : 16;
    int txns     = (argc > 3) ? atoi(argv[3]) : 50;
    if (nworkers <= 0) { nworkers = 4; }
    if (ntasks   <= 0) { ntasks = 16; }
    if (txns     <= 0) { txns = 50; }

    /* create the shared database file with the record initialized to 0 */
    char path[] = "/tmp/dlsm_db_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }
    unlink(path);
    int64_t zero = 0;
    if (pwrite(fd, &zero, sizeof zero, 0) != (ssize_t)sizeof zero) { perror("pwrite"); return 2; }
    fsync(fd);

    printf("== dlsm shared-file DB: concurrent read-modify-write under a green-thread lock ==\n");
    printf("workers=%d  tasks=%d  txns/task=%d  expected final record=%d\n\n",
           nworkers, ntasks, txns, ntasks * txns);
    fflush(stdout);

    dlsm_io *io = dlsm_io_new(256);
    if (!io) { fprintf(stderr, "dlsm_io_new failed\n"); return 2; }
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(nworkers, 0);
    dlsm_gt_mutex mtx;
    dlsm_gt_mutex_init(&mtx, &GT_OPS);

    txn_ctx *ctx = calloc((size_t)ntasks, sizeof *ctx);
    for (int i = 0; i < ntasks; i++) {
        ctx[i] = (txn_ctx){ .io = io, .mtx = &mtx, .fd = fd, .id = i, .txns = txns, .ok = 0 };
        dlsm_gt_spawn(rt, txn_task, &ctx[i]);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    dlsm_gt_run(rt);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    /* read the final record back (synchronously, after the runtime stopped) */
    int64_t final = -1;
    (void)!pread(fd, &final, sizeof final, 0);

    int all_ok = 1;
    for (int i = 0; i < ntasks; i++) { if (!ctx[i].ok) { all_ok = 0; } }

    dlsm_gt_runtime_free(rt);
    dlsm_io_free(io);
    free(ctx);
    close(fd);

    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    long expected = (long)ntasks * txns;
    printf("-- summary --\n");
    printf("final record   = %lld   (expected %ld)\n", (long long)final, expected);
    printf("lock yields    = %ld   (green threads that parked on the lock and were switched back)\n",
           atomic_load(&g_contended));
    printf("workers=%d  elapsed=%.2f ms\n", nworkers, ms);

    if (!all_ok)            { printf("RESULT: FAIL (a transaction errored)\n"); return 1; }
    if (final != expected)  { printf("RESULT: FAIL (lost updates — lock did not serialize)\n"); return 1; }
    printf("RESULT: PASS (no lost updates; lock + yield/switch-back worked)\n");
    return 0;
}
