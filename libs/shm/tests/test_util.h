#ifndef DLSM_TEST_UTIL_H
#define DLSM_TEST_UTIL_H

#include <stdint.h>
#include <pthread.h>

/* xorshift64* — deterministic, seedable. */
typedef struct { uint64_t s; } rng_t;
static inline void rng_seed(rng_t *r, uint64_t seed) { r->s = seed ? seed : 0x9E3779B97F4A7C15ULL; }
static inline uint64_t rng_next(rng_t *r) {
    uint64_t x = r->s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}
/* uniform in [lo, hi] inclusive */
static inline uint64_t rng_range(rng_t *r, uint64_t lo, uint64_t hi) {
    return lo + (rng_next(r) % (hi - lo + 1));
}

/* Start n threads on fn, wait for all. */
static inline void thread_team(int n, void *(*fn)(void *), void **args) {
    pthread_t th[64];
    for (int i = 0; i < n; i++) { pthread_create(&th[i], NULL, fn, args[i]); }
    for (int i = 0; i < n; i++) { pthread_join(th[i], NULL); }
}

#endif
