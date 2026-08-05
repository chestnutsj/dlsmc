/* L3/L4: dlsm_gt_mutex mutual-exclusion stress.
 *
 * dlsm_gt_mutex is suspension-agnostic (dlsm_suspend_ops). Here we drive it with
 * a *pthread-backed* suspender — each OS thread plays the role of one cooperative
 * context: park() blocks on its own condvar, unpark(h) signals that thread's
 * condvar (permit-based, so no lost wakeups). N threads contend on one mutex
 * around a non-atomic counter; an exact final value proves mutual exclusion, and
 * TSAN validates the ordering. (This keeps the test independent of greenthread;
 * the real greenthread integration is exercised by examples/gt_io_db.) */
#include "dlsm/sync.h"
#include <pthread.h>
#include <stdio.h>

#define NTHREAD 8
#define ITERS   20000

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t  cv;
    int             permit;
} parker;

/* This fixture models one suspend context per physical pthread. */
static __thread parker *tls_parker; /* DLSM_GT_NATIVE_TLS_ALLOWED */

static void *sp_current(void) { return tls_parker; }
static void  sp_park(void) {
    parker *p = tls_parker;
    pthread_mutex_lock(&p->m);
    while (!p->permit) { pthread_cond_wait(&p->cv, &p->m); }
    p->permit = 0;
    pthread_mutex_unlock(&p->m);
}
static void sp_unpark(void *h) {
    parker *p = (parker *)h;
    pthread_mutex_lock(&p->m);
    p->permit = 1;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->m);
}
static const dlsm_suspend_ops SP_OPS = {
    .current = sp_current, .park = sp_park, .unpark = sp_unpark,
    .park_until = NULL
};

static dlsm_gt_mutex g_mtx;
static long          g_counter; /* non-atomic, protected by g_mtx */

static void *worker(void *arg) {
    parker self;
    pthread_mutex_init(&self.m, NULL);
    pthread_cond_init(&self.cv, NULL);
    self.permit = 0;
    tls_parker = &self;
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        dlsm_gt_mutex_lock(&g_mtx);
        g_counter++;
        dlsm_gt_mutex_unlock(&g_mtx);
    }
    pthread_mutex_destroy(&self.m);
    pthread_cond_destroy(&self.cv);
    return NULL;
}

int main(void) {
    dlsm_gt_mutex_init(&g_mtx, &SP_OPS);
    g_counter = 0;
    pthread_t th[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) { pthread_create(&th[i], NULL, worker, NULL); }
    for (int i = 0; i < NTHREAD; i++) { pthread_join(th[i], NULL); }
    long expect = (long)NTHREAD * ITERS;
    if (g_counter != expect) {
        fprintf(stderr, "gt_mutex: counter %ld != %ld (mutual exclusion violated)\n",
                g_counter, expect);
        return 1;
    }
    printf("gt_mutex stress ok: %ld increments under park-based mutex\n", g_counter);
    return 0;
}
