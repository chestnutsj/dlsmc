/* Multi-threaded stress / oracle for dlsm-index P2 (concurrency + GC).
 *
 * N OS threads share ONE index and hammer it concurrently with
 * insert/update/delete/get. Each thread owns a disjoint key range, so the
 * expected value of every key is deterministic (last op by its owner) even
 * though threads collide on shared structure: adjacent keys land in the same
 * leaf, and splits/consolidations run concurrently with other threads' writes.
 * Each thread verifies its own range against a private model; main re-verifies
 * the whole key space after join.
 *
 * This is the ROADMAP P2 gate ("8 线程压力通过"); under TSAN it is also the
 * data-race detector for the lock-free read / CAS-install write paths.
 *
 * Usage: stress_index_mt [nthreads=8] [keys_per_thread=1500] [ops_per_thread=30000]
 * Exit: 0 = PASS, non-zero = FAIL.
 */
#include "dlsm/index.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int      g_kpt;        /* keys per thread */
static int      g_ops;        /* ops per thread */
static dlsm_index *g_idx;
static int     *g_present;    /* [nthreads * kpt] reference model */
static uint64_t *g_off;

static void mkkey(char *b, int gk) { snprintf(b, 16, "k%010d", gk); }

typedef struct { int tid; int ok; } targ;

static void *worker(void *p) {
    targ *a = (targ *)p;
    uint64_t s = 0x1234567ULL + (uint64_t)a->tid * 0x9E3779B97F4A7C15ULL;
    int base = a->tid * g_kpt;
    a->ok = 1;
    char k[16];
    for (int n = 0; n < g_ops; n++) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        uint64_t r = s * 0x2545F4914F6CDD1DULL;
        int local = (int)(r % (uint64_t)g_kpt);
        int gk = base + local;
        mkkey(k, gk);
        uint32_t pick = (uint32_t)((r >> 33) % 100);
        if (pick < 35) {                                /* delete */
            if (dlsm_index_delete(g_idx, k, strlen(k)) != DLSM_OK) { a->ok = 0; }
            g_present[gk] = 0;
        } else if (pick < 80) {                         /* insert / update */
            uint64_t v = (r | 1u);
            dlsm_delta_pointer dp = { .kind = DLSM_DP_HOT, .offset = v };
            if (dlsm_index_insert(g_idx, k, strlen(k), dp) != DLSM_OK) { a->ok = 0; }
            g_present[gk] = 1; g_off[gk] = v;
        } else {                                        /* get: must match model */
            dlsm_delta_pointer dp;
            dlsm_status st = dlsm_index_get(g_idx, k, strlen(k), &dp);
            if (g_present[gk]) {
                if (st != DLSM_OK || dp.offset != g_off[gk]) { a->ok = 0; }
            } else if (st != DLSM_INDEX_E_NOTFOUND) { a->ok = 0; }
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    int nthreads = (argc > 1) ? atoi(argv[1]) : 8;
    g_kpt        = (argc > 2) ? atoi(argv[2]) : 1500;
    g_ops        = (argc > 3) ? atoi(argv[3]) : 30000;
    if (nthreads <= 0) { nthreads = 8; }
    if (g_kpt <= 0) { g_kpt = 1500; }
    if (g_ops < 0) { g_ops = 30000; }

    int nkeys = nthreads * g_kpt;
    g_idx = dlsm_index_new();
    g_present = calloc((size_t)nkeys, sizeof *g_present);
    g_off = calloc((size_t)nkeys, sizeof *g_off);
    if (!g_idx || !g_present || !g_off) { fprintf(stderr, "alloc failed\n"); return 2; }

    pthread_t *th = calloc((size_t)nthreads, sizeof *th);
    targ *args = calloc((size_t)nthreads, sizeof *args);
    for (int i = 0; i < nthreads; i++) { args[i] = (targ){ i, 0 }; pthread_create(&th[i], NULL, worker, &args[i]); }
    int all_ok = 1;
    for (int i = 0; i < nthreads; i++) { pthread_join(th[i], NULL); if (!args[i].ok) { all_ok = 0; } }

    /* whole-key-space verification after all writers quiesced */
    uint64_t live = 0, mism = 0;
    char k[16];
    for (int gk = 0; gk < nkeys; gk++) {
        mkkey(k, gk);
        dlsm_delta_pointer dp;
        dlsm_status st = dlsm_index_get(g_idx, k, strlen(k), &dp);
        if (g_present[gk]) { live++; if (st != DLSM_OK || dp.offset != g_off[gk]) { mism++; } }
        else if (st != DLSM_INDEX_E_NOTFOUND) { mism++; }
    }

    dlsm_index_stats st;
    dlsm_index_stats_get(g_idx, &st);
    printf("== dlsm-index MT stress ==\n");
    printf("threads=%d keys=%d ops/thread=%d  height=%u live=%llu (expect %llu)\n",
           nthreads, nkeys, g_ops, st.height, (unsigned long long)st.live_keys, (unsigned long long)live);
    printf("consolidations=%llu leaf_splits=%llu internal_splits=%llu  mismatches=%llu  in-flight ok=%d\n",
           (unsigned long long)st.consolidations, (unsigned long long)st.leaf_splits,
           (unsigned long long)st.internal_splits, (unsigned long long)mism, all_ok);

    int ok = 1;
    if (!all_ok)             { printf("FAIL: an in-flight get/op disagreed with the model\n"); ok = 0; }
    if (mism != 0)           { printf("FAIL: %llu post-run mismatches\n", (unsigned long long)mism); ok = 0; }
    if (st.live_keys != live){ printf("FAIL: live_keys disagrees with model\n"); ok = 0; }
    if (st.leaf_splits == 0) { printf("FAIL: no leaf splits (structure not exercised)\n"); ok = 0; }

    dlsm_index_free(g_idx);
    free(g_present); free(g_off); free(th); free(args);
    if (!ok) { return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
