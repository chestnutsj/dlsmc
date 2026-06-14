#include "dlsm/sync.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>

#define NWAITERS 8

static dlsm_event event;
static _Atomic int ready;
static _Atomic int woke;

static void *waiter(void *arg) {
    (void)arg;
    uint32_t observed = dlsm_event_snapshot(&event);
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
    if (dlsm_event_wait(&event, observed) == DLSM_OK) {
        atomic_fetch_add_explicit(&woke, 1, memory_order_release);
    }
    return NULL;
}

int main(void) {
    dlsm_event_init(&event);
    atomic_store(&ready, 0);
    atomic_store(&woke, 0);
    pthread_t threads[NWAITERS];
    for (int i = 0; i < NWAITERS; i++) {
        if (pthread_create(&threads[i], NULL, waiter, NULL) != 0) { return 1; }
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) != NWAITERS) {
        sched_yield();
    }
    dlsm_event_notify_all(&event);
    for (int i = 0; i < NWAITERS; i++) { pthread_join(threads[i], NULL); }
    if (atomic_load_explicit(&woke, memory_order_acquire) != NWAITERS) { return 2; }
    printf("event stress ok: %d waiters resumed\n", NWAITERS);
    return 0;
}
