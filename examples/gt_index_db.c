/* Example: a mini storage-engine slice — Bw-Tree INDEX + append-only Delta Log,
 * driven concurrently by green threads under a park-based lock.
 *
 * This is the database-style sibling of gt_io_db.c, but with a real index:
 *
 *   - dlsm-index (Bw-Tree, ROADMAP P1) is the in-memory primary index. It maps
 *     account key -> DeltaPointer{HOT, offset}, i.e. the byte offset of that
 *     row's latest record in the data file. The index never stores the value
 *     body — exactly the DLSM split (bwtree.md §1).
 *   - the data file is an append-only "Delta Log": every write lands at a fresh
 *     offset (copy-on-write, never in place — bwtree.md §8.2), made durable with
 *     io_uring fdatasync.
 *   - many green threads, across several worker cores, each run transactions:
 *
 *         LOCK -> idx.get(key) -> io.read_at(offset) -> balance+1
 *              -> off = append_cursor -> io.write_at(off) -> io.fdatasync
 *              -> idx.insert(key, {HOT, off}) -> UNLOCK
 *
 * Why the lock: the P1 index is single-threaded (P2 adds CAS install + epoch
 * GC), so concurrent access must serialize. The lock is dlsm-sync's park-based
 * dlsm_gt_mutex — a contended green thread *yields* (parks, freeing its worker)
 * and is *switched back* (unparked) when the holder releases. Crucially the lock
 * is held across io_uring ops that themselves park; a spin lock there would burn
 * the worker and could deadlock the cooperative scheduler (architecture.md §7.3).
 *
 * Correctness proof: with the lock, each account's final balance == the number
 * of transactions that touched it, and the sum over accounts == total
 * transactions (no lost updates). The index, read back row by row through the
 * Delta Log, must agree with an independent reference model.
 *
 * Usage: gt_index_db [nworkers=4] [ntasks=16] [txns_per_task=100] [naccts=256]
 */
#define _GNU_SOURCE
#include "dlsm/io.h"
#include "dlsm/greenthread.h"
#include "dlsm/sync.h"
#include "dlsm/index.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Bind dlsm_gt_mutex to the greenthread runtime; count parks to report how many
 * times a green thread yielded on the lock and was switched back. */
static _Atomic long g_yields;
static void *gt_current(void) { return dlsm_gt_self(); }
static void  gt_park_counted(void) {
    atomic_fetch_add_explicit(&g_yields, 1, memory_order_relaxed);
    dlsm_gt_park();
}
static void  gt_unpark_one(void *h) { dlsm_gt_unpark((dlsm_gt_task *)h); }
static const dlsm_suspend_ops GT_OPS = { gt_current, gt_park_counted, gt_unpark_one };

typedef struct {
    dlsm_io       *io;
    dlsm_gt_mutex *mtx;
    dlsm_index    *idx;
    int            fd;
    int            naccts;
    uint64_t      *expected;       /* per-account update count (reference model) */
    uint64_t       append_cursor;  /* next free offset in the Delta Log */
} db_ctx;

typedef struct { db_ctx *db; int txns; uint64_t seed; int ok; } task_ctx;

static uint64_t xr(uint64_t *s) {
    *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
    return *s * 0x2545F4914F6CDD1DULL;
}

static void txn_task(void *arg) {
    task_ctx *c = (task_ctx *)arg;
    db_ctx *db = c->db;
    c->ok = 1;
    for (int i = 0; i < c->txns; i++) {
        int acct = (int)(xr(&c->seed) % (uint64_t)db->naccts);
        char key[16];
        int klen = snprintf(key, sizeof key, "acct%04d", acct);

        dlsm_gt_mutex_lock(db->mtx);                /* serialize index + Delta Log */

        int64_t bal = 0;
        dlsm_delta_pointer dp;
        if (dlsm_index_get(db->idx, key, (size_t)klen, &dp) == DLSM_OK) {
            if (dlsm_io_read_at(db->io, db->fd, &bal, sizeof bal, (off_t)dp.offset) != (ssize_t)sizeof bal) {
                c->ok = 0; dlsm_gt_mutex_unlock(db->mtx); return;
            }
        }
        bal += 1;                                   /* modify */
        uint64_t off = db->append_cursor;           /* append-only: fresh offset */
        db->append_cursor += sizeof bal;
        if (dlsm_io_write_at(db->io, db->fd, &bal, sizeof bal, (off_t)off) != (ssize_t)sizeof bal) {
            c->ok = 0; dlsm_gt_mutex_unlock(db->mtx); return;
        }
        if (dlsm_io_fdatasync(db->io, db->fd) != 0) { c->ok = 0; dlsm_gt_mutex_unlock(db->mtx); return; }
        dlsm_delta_pointer hot = { .kind = DLSM_DP_HOT, .file_id = 0, .offset = off };
        if (dlsm_index_insert(db->idx, key, (size_t)klen, hot) != DLSM_OK) {
            c->ok = 0; dlsm_gt_mutex_unlock(db->mtx); return;
        }
        db->expected[acct] += 1;

        dlsm_gt_mutex_unlock(db->mtx);
    }
}

int main(int argc, char **argv) {
    int nworkers = (argc > 1) ? atoi(argv[1]) : 4;
    int ntasks   = (argc > 2) ? atoi(argv[2]) : 16;
    int txns     = (argc > 3) ? atoi(argv[3]) : 100;
    int naccts   = (argc > 4) ? atoi(argv[4]) : 256;
    if (nworkers <= 0) { nworkers = 4; }
    if (ntasks   <= 0) { ntasks = 16; }
    if (txns     <= 0) { txns = 100; }
    if (naccts   <= 0) { naccts = 256; }

    char path[] = "/tmp/dlsm_idxdb_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }
    unlink(path);

    printf("== dlsm mini-engine: Bw-Tree index + append-only Delta Log, concurrent under a green-thread lock ==\n");
    printf("workers=%d  tasks=%d  txns/task=%d  accounts=%d  total txns=%d\n\n",
           nworkers, ntasks, txns, naccts, ntasks * txns);
    fflush(stdout);

    dlsm_io *io = dlsm_io_new(256);
    if (!io) { fprintf(stderr, "dlsm_io_new failed\n"); return 2; }
    dlsm_index *idx = dlsm_index_new();
    if (!idx) { fprintf(stderr, "dlsm_index_new failed\n"); return 2; }
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(nworkers, 0);
    dlsm_gt_mutex mtx;
    dlsm_gt_mutex_init(&mtx, &GT_OPS);

    db_ctx db = { io, &mtx, idx, fd, naccts, calloc((size_t)naccts, sizeof(uint64_t)), 0 };
    task_ctx *ctx = calloc((size_t)ntasks, sizeof *ctx);
    for (int i = 0; i < ntasks; i++) {
        ctx[i] = (task_ctx){ .db = &db, .txns = txns, .seed = 0x1000 + (uint64_t)i * 0x9E37, .ok = 0 };
        dlsm_gt_spawn(rt, txn_task, &ctx[i]);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    dlsm_gt_run(rt);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    int all_ok = 1;
    for (int i = 0; i < ntasks; i++) { if (!ctx[i].ok) { all_ok = 0; } }

    /* verify the durable index against the reference model (synchronously, after
     * the runtime stopped): every account's latest record == its update count. */
    uint64_t sum = 0, expected_sum = 0, mism = 0;
    for (int a = 0; a < naccts; a++) {
        char key[16];
        int klen = snprintf(key, sizeof key, "acct%04d", a);
        expected_sum += db.expected[a];
        dlsm_delta_pointer dp;
        dlsm_status st = dlsm_index_get(idx, key, (size_t)klen, &dp);
        if (db.expected[a] == 0) {
            if (st != DLSM_INDEX_E_NOTFOUND) { mism++; }
            continue;
        }
        int64_t bal = -1;
        if (st != DLSM_OK || pread(fd, &bal, sizeof bal, (off_t)dp.offset) != (ssize_t)sizeof bal) { mism++; continue; }
        if ((uint64_t)bal != db.expected[a]) { mism++; }
        sum += (uint64_t)bal;
    }

    dlsm_index_stats s;
    dlsm_index_stats_get(idx, &s);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    long total = (long)ntasks * txns;

    printf("-- summary --\n");
    printf("balances sum   = %llu   (expected %llu)\n", (unsigned long long)sum, (unsigned long long)expected_sum);
    printf("index          : height=%u  live_keys=%llu  leaf_splits=%llu  consolidations=%llu\n",
           s.height, (unsigned long long)s.live_keys,
           (unsigned long long)s.leaf_splits, (unsigned long long)s.consolidations);
    printf("delta log      : %llu bytes appended (%ld records)\n",
           (unsigned long long)db.append_cursor, total);
    printf("lock yields    = %ld   (green threads that parked on the lock and were switched back)\n",
           atomic_load(&g_yields));
    printf("workers=%d  elapsed=%.2f ms\n", nworkers, ms);

    int rc = 0;
    if (!all_ok)                  { printf("RESULT: FAIL (a transaction errored)\n"); rc = 1; }
    else if (mism != 0)           { printf("RESULT: FAIL (%llu accounts disagree with the model)\n", (unsigned long long)mism); rc = 1; }
    else if (sum != expected_sum) { printf("RESULT: FAIL (lost updates)\n"); rc = 1; }
    else if (expected_sum != (uint64_t)total) { printf("RESULT: FAIL (model lost transactions)\n"); rc = 1; }
    else { printf("RESULT: PASS (index + Delta Log consistent; no lost updates; lock yield/switch-back worked)\n"); }

    dlsm_gt_runtime_free(rt);
    dlsm_io_free(io);
    dlsm_index_free(idx);
    free(db.expected);
    free(ctx);
    close(fd);
    return rc;
}
