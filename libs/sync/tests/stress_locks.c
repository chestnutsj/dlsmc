/* L3/L4: mutual-exclusion stress for ticket and MCS locks. A shared,
 * non-atomic counter is incremented only under the lock; if the lock provides
 * mutual exclusion and proper happens-before, the final value is exact and
 * ThreadSanitizer reports no race on the counter. */
#include "dlsm/sync.h"
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>

#define NTHREAD 8
#define ITERS   200000

static dlsm_ticket_lock g_ticket;
static dlsm_mcs_lock    g_mcs;
static long             g_counter; /* deliberately non-atomic */

static void *ticket_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        dlsm_ticket_lock_acquire(&g_ticket);
        g_counter++;
        dlsm_ticket_lock_release(&g_ticket);
    }
    return NULL;
}

static void *mcs_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        dlsm_mcs_node node;
        dlsm_mcs_lock_acquire(&g_mcs, &node);
        g_counter++;
        dlsm_mcs_lock_release(&g_mcs, &node);
    }
    return NULL;
}

static int run(const char *name, void *(*fn)(void *)) {
    g_counter = 0;
    pthread_t th[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) { pthread_create(&th[i], NULL, fn, NULL); }
    for (int i = 0; i < NTHREAD; i++) { pthread_join(th[i], NULL); }
    long expect = (long)NTHREAD * ITERS;
    if (g_counter != expect) {
        fprintf(stderr, "%s: counter %ld != %ld (mutual exclusion violated)\n",
                name, g_counter, expect);
        return 1;
    }
    printf("%s ok: %ld increments\n", name, g_counter);
    return 0;
}

int main(void) {
    dlsm_ticket_init(&g_ticket);
    dlsm_mcs_init(&g_mcs);
    if (run("ticket", ticket_worker)) { return 1; }
    if (run("mcs", mcs_worker)) { return 1; }
    return 0;
}
