/* Stress / large-scale oracle for dlsm-index (single-threaded by design in P1).
 *
 * Builds a tall multi-level tree (forcing INTERNAL splits + recursive root
 * growth, not just leaf splits), then runs a long insert/update/delete churn,
 * checking every step against a reference model. Run under ASAN/UBSAN this is
 * the memory-safety net for the split/consolidate/free ownership transfers.
 *
 * Usage: stress_index [nkeys=40000] [churn_ops=120000]
 * Exit: 0 = PASS, non-zero = FAIL.
 */
#include "dlsm/index.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0xD1CE5BADC0FFEE11ULL;
static uint64_t rng(void) {
    rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27;
    return rs * 0x2545F4914F6CDD1DULL;
}
static void mkkey(char *buf, int i) { snprintf(buf, 16, "key%08d", i); }

static int getoff(dlsm_index *t, const char *k, uint64_t *off) {
    dlsm_delta_pointer dp;
    dlsm_status st = dlsm_index_get(t, k, strlen(k), &dp);
    if (st != DLSM_OK) { return (int)st; }
    *off = dp.offset;
    return 0;
}

int main(int argc, char **argv) {
    int nkeys = (argc > 1) ? atoi(argv[1]) : 40000;
    int churn = (argc > 2) ? atoi(argv[2]) : 120000;
    if (nkeys <= 0) { nkeys = 40000; }
    if (churn < 0) { churn = 0; }

    dlsm_index *t = dlsm_index_new();
    if (!t) { fprintf(stderr, "new failed\n"); return 2; }
    int      *present = calloc((size_t)nkeys, sizeof *present);
    uint64_t *off = calloc((size_t)nkeys, sizeof *off);
    if (!present || !off) { fprintf(stderr, "alloc failed\n"); return 2; }

    char k[16];
    /* phase 1: load all keys in scrambled order -> deep tree */
    for (int n = 0; n < nkeys; n++) {
        int i = (int)(rng() % (uint64_t)nkeys);
        mkkey(k, i);
        uint64_t v = rng() | 1u;
        if (dlsm_index_insert(t, k, strlen(k), (dlsm_delta_pointer){ .kind = DLSM_DP_HOT, .offset = v }) != DLSM_OK) {
            fprintf(stderr, "insert failed\n"); return 1;
        }
        present[i] = 1; off[i] = v;
    }

    /* phase 2: random insert/update/delete churn */
    for (int n = 0; n < churn; n++) {
        int i = (int)(rng() % (uint64_t)nkeys);
        mkkey(k, i);
        if (rng() % 100 < 40) {
            if (dlsm_index_delete(t, k, strlen(k)) != DLSM_OK) { fprintf(stderr, "delete failed\n"); return 1; }
            present[i] = 0;
        } else {
            uint64_t v = rng() | 1u;
            if (dlsm_index_insert(t, k, strlen(k), (dlsm_delta_pointer){ .kind = DLSM_DP_HOT, .offset = v }) != DLSM_OK) {
                fprintf(stderr, "update failed\n"); return 1;
            }
            present[i] = 1; off[i] = v;
        }
    }

    /* full-scan verification against the reference model */
    uint64_t live = 0, mism = 0;
    for (int i = 0; i < nkeys; i++) {
        mkkey(k, i);
        uint64_t got = 0;
        int st = getoff(t, k, &got);
        if (present[i]) {
            live++;
            if (st != 0 || got != off[i]) { mism++; }
        } else if (st != DLSM_INDEX_E_NOTFOUND) {
            mism++;
        }
    }

    dlsm_index_stats s;
    dlsm_index_stats_get(t, &s);
    printf("== dlsm-index stress ==\n");
    printf("keys=%d churn=%d  height=%u live=%llu (expect %llu)\n",
           nkeys, churn, s.height, (unsigned long long)s.live_keys, (unsigned long long)live);
    printf("consolidations=%llu leaf_splits=%llu internal_splits=%llu mismatches=%llu\n",
           (unsigned long long)s.consolidations, (unsigned long long)s.leaf_splits,
           (unsigned long long)s.internal_splits, (unsigned long long)mism);

    int ok = 1;
    if (mism != 0)              { printf("FAIL: %llu key mismatches\n", (unsigned long long)mism); ok = 0; }
    if (s.live_keys != live)    { printf("FAIL: live_keys disagrees with model\n"); ok = 0; }
    if (s.internal_splits == 0) { printf("FAIL: tree never split an internal node (raise nkeys)\n"); ok = 0; }
    if (s.height < 3)           { printf("FAIL: tree shallower than expected (height=%u)\n", s.height); ok = 0; }

    dlsm_index_free(t);
    free(present);
    free(off);
    if (!ok) { return 1; }
    printf("RESULT: PASS\n");
    return 0;
}
