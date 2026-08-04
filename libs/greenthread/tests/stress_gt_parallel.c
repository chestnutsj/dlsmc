/* L3/L4: many green threads run across multiple VPs, hammering a
 * shared atomic counter with yields in between. Correct final total proves the
 * scheduler runs and resumes tasks across VPs without losing or duplicating
 * them; TSAN validates the cross-VP happens-before on resume. */
#include "dlsm/greenthread.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>

#define NTASKS 200
#define PER    2000

static _Atomic long g_counter;

static void vp_task(void *arg) {
    (void)arg;
    for (int i = 0; i < PER; i++) {
        atomic_fetch_add_explicit(&g_counter, 1, memory_order_relaxed);
        if ((i & 7) == 0) { dlsm_gt_yield(); }
    }
}

int main(void) {
    atomic_store(&g_counter, 0);
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(4, 0); /* 4 VPs */
    if (!rt) { fprintf(stderr, "runtime_new failed\n"); return 1; }
    for (int i = 0; i < NTASKS; i++) { dlsm_gt_spawn(rt, vp_task, NULL); }
    dlsm_gt_run(rt);
    dlsm_gt_runtime_free(rt);

    long got = atomic_load(&g_counter);
    long expect = (long)NTASKS * PER;
    if (got != expect) {
        fprintf(stderr, "counter %ld != %ld\n", got, expect);
        return 1;
    }
    printf("gt parallel ok: %ld increments across %d tasks / 4 VPs\n", got, NTASKS);
    return 0;
}
