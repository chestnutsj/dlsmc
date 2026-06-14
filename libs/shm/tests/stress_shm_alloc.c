#include "dlsm/shm.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define NAME "/dlsm_stress_alloc"
#define SIZE (64u << 20)        /* 64 MiB */
#define NTHREAD 8
#define PER_THREAD 50000
#define BLK 64                  /* fixed block size for overlap check */

typedef struct { dlsm_shm *s; uintptr_t *out; int n; } job_t;

static void *worker(void *arg) {
    job_t *j = (job_t *)arg;
    for (int i = 0; i < j->n; i++) {
        void *p = dlsm_shm_alloc(j->s, BLK, 16);
        if (!p) { j->out[i] = 0; continue; }
        j->out[i] = (uintptr_t)p;
    }
    return NULL;
}

static int cmp_uptr(const void *a, const void *b) {
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return (x > y) - (x < y);
}

int main(void) {
    shm_unlink(NAME);
    dlsm_shm *s = NULL;
    if (dlsm_shm_create_or_recover(NAME, SIZE, &s) != DLSM_OK) {
        fprintf(stderr, "create failed\n"); return 1;
    }
    static uintptr_t bufs[NTHREAD][PER_THREAD];
    job_t jobs[NTHREAD];
    void *args[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) {
        jobs[i] = (job_t){ .s = s, .out = bufs[i], .n = PER_THREAD };
        args[i] = &jobs[i];
    }
    thread_team(NTHREAD, worker, args);

    /* gather non-null pointers */
    static uintptr_t all[NTHREAD * PER_THREAD];
    int n = 0;
    uintptr_t base = (uintptr_t)dlsm_shm_base(s);
    uintptr_t end  = base + dlsm_shm_capacity(s);
    for (int t = 0; t < NTHREAD; t++) {
        for (int i = 0; i < PER_THREAD; i++) {
            uintptr_t p = bufs[t][i];
            if (!p) { continue; }
            if (p < base || p + BLK > end || (p % 16) != 0) {
                fprintf(stderr, "invariant violated: bad ptr %p\n", (void *)p);
                return 2;
            }
            all[n++] = p;
        }
    }
    qsort(all, n, sizeof(uintptr_t), cmp_uptr);
    for (int i = 1; i < n; i++) {
        if (all[i] < all[i - 1] + BLK) {
            fprintf(stderr, "overlap: %p and %p\n", (void *)all[i-1], (void *)all[i]);
            return 3;
        }
    }
    dlsm_shm_detach(s);
    shm_unlink(NAME);
    printf("stress ok: %d allocations, no overlap\n", n);
    return 0;
}
