/* L3/L4: concurrent EBR stress. Readers dereference a shared object inside an
 * epoch; writers swap in new objects and retire the old ones. If reclamation
 * ever frees an object a reader can still see, ASAN/TSAN flag a use-after-free.
 * The MAGIC check gives an additional logical tripwire. */
#include "dlsm/sync.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define READERS 6
#define WRITERS 2
#define ITERS   100000
#define MAGIC   0xABCDEF0123456789ULL

typedef struct { uint64_t magic; } obj_t;

static dlsm_ebr g_ebr;
static _Atomic(obj_t *) g_slot;
static _Atomic int g_fail;

static obj_t *make_obj(void) {
    obj_t *o = malloc(sizeof(obj_t));
    o->magic = MAGIC;
    return o;
}
static void free_obj(void *p) {
    obj_t *o = (obj_t *)p;
    o->magic = 0; /* poison so a late reader trips the check (and ASAN catches UAF) */
    free(o);
}

static void *reader(void *arg) {
    (void)arg;
    int s;
    if (dlsm_ebr_register(&g_ebr, &s) != DLSM_OK) { atomic_store(&g_fail, 1); return NULL; }
    for (int i = 0; i < ITERS; i++) {
        dlsm_ebr_enter(&g_ebr, s);
        obj_t *o = atomic_load_explicit(&g_slot, memory_order_acquire);
        if (o->magic != MAGIC) { atomic_store(&g_fail, 1); }
        dlsm_ebr_exit(&g_ebr, s);
    }
    dlsm_ebr_unregister(&g_ebr, s);
    return NULL;
}

static void *writer(void *arg) {
    (void)arg;
    int s;
    if (dlsm_ebr_register(&g_ebr, &s) != DLSM_OK) { atomic_store(&g_fail, 1); return NULL; }
    for (int i = 0; i < ITERS; i++) {
        obj_t *fresh = make_obj();
        obj_t *old = atomic_exchange_explicit(&g_slot, fresh, memory_order_acq_rel);
        if (dlsm_ebr_retire(&g_ebr, old, free_obj) != DLSM_OK) {
            atomic_store(&g_fail, 1);
        }
        if ((i & 0x3f) == 0) { dlsm_ebr_try_advance(&g_ebr); }
    }
    dlsm_ebr_unregister(&g_ebr, s);
    return NULL;
}

int main(void) {
    dlsm_ebr_init(&g_ebr);
    atomic_store(&g_slot, make_obj());
    atomic_store(&g_fail, 0);

    pthread_t r[READERS], w[WRITERS];
    for (int i = 0; i < WRITERS; i++) { pthread_create(&w[i], NULL, writer, NULL); }
    for (int i = 0; i < READERS; i++) { pthread_create(&r[i], NULL, reader, NULL); }
    for (int i = 0; i < READERS; i++) { pthread_join(r[i], NULL); }
    for (int i = 0; i < WRITERS; i++) { pthread_join(w[i], NULL); }

    /* drain: no active readers now, so a few advances reclaim everything */
    for (int i = 0; i < 4; i++) { dlsm_ebr_try_advance(&g_ebr); }
    free(atomic_load(&g_slot)); /* the final live object */

    if (atomic_load(&g_fail)) { fprintf(stderr, "EBR stress: saw poisoned object\n"); return 1; }
    printf("ebr stress ok: %d readers x %d writers x %d iters\n", READERS, WRITERS, ITERS);
    return 0;
}
