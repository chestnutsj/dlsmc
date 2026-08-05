/* dlsm-index — concurrent in-memory Bw-Tree (ROADMAP 主线 B, P2).
 *
 * P1 was single-threaded; P2 makes it safe for many threads at once:
 *
 *   - Reads are lock-free, protected by epoch-based reclamation (dlsm-sync EBR):
 *     a reader enters an epoch, walks atomically-loaded chain heads, exits.
 *   - Writes install a Delta onto a leaf's chain head with a single CAS and
 *     retry on contention (no lock on the fast path). A deep chain consolidates
 *     by CAS-swapping a fresh base; the old chain is retired to EBR, never freed
 *     while a reader might hold it.
 *   - Structure modifications (split) take ONE tree-global SMO lock so parent /
 *     root mutations are serialized; internal nodes are immutable and replaced
 *     copy-on-write + atomic swap, so concurrent readers always see a consistent
 *     snapshot. To stay correct during an in-progress split WITHOUT going back
 *     up the tree, every node carries a high-fence key + right-sibling link
 *     (B-link): a reader/writer whose key has moved past high_fence hops right.
 *
 * This is a documented P2 model: point reads/writes are lock-free (the OLTP
 * fast path — the spec's "no central lock / no page latch" property holds for
 * them); SMOs are serialized by a single lock (the spec's delta-driven 3-step
 * split is the further refinement). Merge and hot-row MCS downgrade build on
 * top of this core. The mapping table is a fixed-size array (never reallocated,
 * so reads never race a resize); PIDs are allocated only under the SMO lock. */
#include "dlsm/index.h"
#include "dlsm/sync.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t pid_t_;
#define PID_NULL 0xFFFFFFFFu

#define DLSM_CONSOLIDATE_DEPTH 8
#define DLSM_CHAIN_CAP         24   /* hard cap: writers drain (not append) past this */
#define DLSM_LEAF_MAX          32
#define DLSM_INTERNAL_MAX      32
#define DLSM_MAP_CAP           (1u << 18)   /* up to 256K nodes (fixed, race-free) */
#define DLSM_MAX_DEPTH         32

typedef enum { NODE_BASE_LEAF, NODE_BASE_INTERNAL, NODE_DELTA } node_kind;
typedef struct chain { node_kind kind; } chain;
typedef enum { OP_INSERT, OP_UPDATE, OP_DELETE } delta_op;

typedef struct {
    node_kind kind;        /* NODE_DELTA */
    delta_op  op;
    uint32_t  depth;
    chain    *next;
    uint8_t  *key;
    uint32_t  klen;
    dlsm_delta_pointer val;
} delta;

typedef struct { uint8_t *key; uint32_t klen; dlsm_delta_pointer val; } leaf_ent;
typedef struct { uint8_t *sep; uint32_t slen; pid_t_ child; } idx_ent;

/* Base nodes are immutable once published; an SMO replaces a node by building a
 * fresh one and atomically swapping the mapping entry. fences + right_sibling
 * give the B-link invariant. */
typedef struct {
    node_kind kind;
    uint8_t   level;
    uint32_t  num, cap;
    leaf_ent *lents;       /* leaf */
    idx_ent  *ients;       /* internal (ients[0].sep == NULL is the -inf child) */
    uint8_t  *low_fence;  uint32_t low_len;   /* NULL = -inf */
    uint8_t  *high_fence; uint32_t high_len;  /* NULL = +inf */
    pid_t_    right_sibling;                   /* PID_NULL if none */
} base;

typedef struct {
    _Atomic(chain *) head;
    _Atomic int      consolidating; /* non-spin try-flag: one consolidator per node */
} mentry;

struct dlsm_index {
    mentry           *map;       /* fixed [DLSM_MAP_CAP]; never moves */
    _Atomic uint32_t  next_pid;  /* bumped only under smo lock */
    _Atomic uint32_t  root;
    dlsm_ebr          ebr;
    _Atomic int       smo_busy;  /* try-lock: 1 structural modifier at a time, no spin */
    _Atomic uint64_t  gc_tick;
    _Atomic uint64_t  consolidations, leaf_splits, internal_splits;
};

/* ---- key compare / dup ---- */
static int keycmp(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen) {
    uint32_t n = alen < blen ? alen : blen;
    int r = n ? memcmp(a, b, n) : 0;
    if (r) { return r; }
    return (alen > blen) - (alen < blen);
}
static uint8_t *keydup(const void *key, uint32_t klen) {
    uint8_t *p = malloc(klen ? klen : 1);
    if (p && klen) { memcpy(p, key, klen); }
    return p;
}
static uint8_t *fdup(const uint8_t *p, uint32_t n) { return p ? keydup(p, n) : NULL; }

/* ---- node lifecycle ---- */
static base *base_leaf_new(void) {
    base *b = calloc(1, sizeof *b);
    if (b) { b->kind = NODE_BASE_LEAF; b->right_sibling = PID_NULL; }
    return b;
}
static base *base_internal_new(uint8_t level) {
    base *b = calloc(1, sizeof *b);
    if (b) { b->kind = NODE_BASE_INTERNAL; b->level = level; b->right_sibling = PID_NULL; }
    return b;
}
static base *tail_base(chain *h) {
    while (h->kind == NODE_DELTA) { h = ((delta *)h)->next; }
    return (base *)h;
}

static void free_chain(chain *c) {
    while (c) {
        if (c->kind == NODE_DELTA) {
            delta *d = (delta *)c;
            chain *next = d->next;
            free(d->key); free(d);
            c = next;
        } else {
            base *b = (base *)c;
            if (b->kind == NODE_BASE_LEAF) {
                for (uint32_t i = 0; i < b->num; i++) { free(b->lents[i].key); }
                free(b->lents);
            } else {
                for (uint32_t i = 0; i < b->num; i++) { free(b->ients[i].sep); }
                free(b->ients);
            }
            free(b->low_fence); free(b->high_fence); free(b);
            c = NULL;
        }
    }
}
static void free_chain_dtor(void *p) { free_chain((chain *)p); }

/* ---- EBR thread registration (lazy, per-thread, auto-unregister) ---- */
static pthread_key_t  g_ebr_key;
static pthread_once_t g_ebr_once = PTHREAD_ONCE_INIT;
typedef struct ebr_reg { dlsm_ebr *e; int slot; struct ebr_reg *next; } ebr_reg;
static void ebr_key_dtor(void *p) {
    ebr_reg *r = p;
    while (r) { ebr_reg *n = r->next; dlsm_ebr_unregister(r->e, r->slot); free(r); r = n; }
}
static void ebr_key_make(void) { /* DLSM_GT_NATIVE_TLS_ALLOWED */
    pthread_key_create(&g_ebr_key, ebr_key_dtor); /* DLSM_GT_NATIVE_TLS_ALLOWED */
}
static int ebr_slot(dlsm_ebr *e) {
    pthread_once(&g_ebr_once, ebr_key_make);
    ebr_reg *r = pthread_getspecific(g_ebr_key); /* DLSM_GT_NATIVE_TLS_ALLOWED */
    for (ebr_reg *x = r; x; x = x->next) { if (x->e == e) { return x->slot; } }
    int slot;
    if (dlsm_ebr_register(e, &slot) != DLSM_OK) { return -1; }
    ebr_reg *nr = malloc(sizeof *nr);
    if (!nr) { dlsm_ebr_unregister(e, slot); return -1; }
    nr->e = e; nr->slot = slot; nr->next = r;
    pthread_setspecific(g_ebr_key, nr); /* DLSM_GT_NATIVE_TLS_ALLOWED */
    return slot;
}
static void ebr_unreg_self(dlsm_ebr *e) {
    ebr_reg *r = pthread_getspecific(g_ebr_key), *prev = NULL; /* DLSM_GT_NATIVE_TLS_ALLOWED */
    for (ebr_reg *x = r; x; prev = x, x = x->next) {
        if (x->e == e) {
            dlsm_ebr_unregister(e, x->slot);
            if (prev) { prev->next = x->next; } else {
                pthread_setspecific(g_ebr_key, x->next); /* DLSM_GT_NATIVE_TLS_ALLOWED */
            }
            free(x);
            return;
        }
    }
}
static void retire_chain(dlsm_index *t, chain *c) { dlsm_ebr_retire(&t->ebr, c, free_chain_dtor); }

/* ---- mapping table ---- */
static mentry *ent(dlsm_index *t, pid_t_ pid) { return &t->map[pid]; }
static chain  *load_head(dlsm_index *t, pid_t_ pid) {
    return atomic_load_explicit(&t->map[pid].head, memory_order_acquire);
}
/* allocate a fresh PID bound to chain c — caller must hold the SMO lock */
static pid_t_ alloc_pid(dlsm_index *t, chain *c) {
    uint32_t p = atomic_fetch_add_explicit(&t->next_pid, 1, memory_order_relaxed);
    if (p >= DLSM_MAP_CAP) { return PID_NULL; }
    atomic_store_explicit(&t->map[p].head, c, memory_order_release);
    return p;
}

dlsm_index *dlsm_index_new(void) {
    dlsm_index *t = calloc(1, sizeof *t);
    if (!t) { return NULL; }
    t->map = calloc(DLSM_MAP_CAP, sizeof *t->map);
    base *root = base_leaf_new();
    if (!t->map || !root) { free(t->map); free(root); free(t); return NULL; }
    dlsm_ebr_init(&t->ebr);
    atomic_store_explicit(&t->map[0].head, (chain *)root, memory_order_relaxed);
    atomic_store_explicit(&t->next_pid, 1, memory_order_relaxed);
    atomic_store_explicit(&t->root, 0, memory_order_relaxed);
    return t;
}

void dlsm_index_free(dlsm_index *t) {
    if (!t) { return; }
    /* No concurrent ops at this point. Drain retired objects (all readers gone),
     * then free the live nodes still in the map. */
    if (ebr_slot(&t->ebr) >= 0) {
        for (int i = 0; i < 16; i++) { dlsm_ebr_try_advance(&t->ebr); }
        ebr_unreg_self(&t->ebr);
    }
    uint32_t n = atomic_load_explicit(&t->next_pid, memory_order_relaxed);
    for (uint32_t p = 0; p < n; p++) {
        chain *c = atomic_load_explicit(&t->map[p].head, memory_order_relaxed);
        if (c) { free_chain(c); }
    }
    free(t->map);
    free(t);
}

/* ---- search helpers ---- */
static int base_find(const base *b, const uint8_t *key, uint32_t klen) {
    int lo = 0, hi = (int)b->num - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int c = keycmp(b->lents[mid].key, b->lents[mid].klen, key, klen);
        if (c == 0) { return mid; }
        if (c < 0) { lo = mid + 1; } else { hi = mid - 1; }
    }
    return -1;
}
static pid_t_ route(const base *in, const uint8_t *k, uint32_t kl) {
    int lo = 1, hi = (int)in->num - 1, res = 0;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (keycmp(in->ients[mid].sep, in->ients[mid].slen, k, kl) <= 0) { res = mid; lo = mid + 1; }
        else { hi = mid - 1; }
    }
    return in->ients[res].child;
}
/* Has `key` moved right past this node's high fence (split in progress)? */
static int past_fence(const base *b, const uint8_t *k, uint32_t kl) {
    return b->high_fence && keycmp(k, kl, b->high_fence, b->high_len) >= 0;
}

dlsm_status dlsm_index_get(const dlsm_index *ct, const void *key, size_t klen, dlsm_delta_pointer *out) {
    dlsm_index *t = (dlsm_index *)ct;
    if (!t || (!key && klen) || !out) { return DLSM_INDEX_E_INVAL; }
    const uint8_t *k = key;
    uint32_t kl = (uint32_t)klen;
    int slot = ebr_slot(&t->ebr);
    dlsm_ebr_enter(&t->ebr, slot);
    dlsm_status st = DLSM_INDEX_E_NOTFOUND;
    pid_t_ pid = atomic_load_explicit(&t->root, memory_order_acquire);
    for (;;) {
        chain *h = load_head(t, pid);
        chain *c = h;
        int matched = 0;
        while (c->kind == NODE_DELTA) {
            delta *d = (delta *)c;
            if (keycmp(d->key, d->klen, k, kl) == 0) {
                if (d->op != OP_DELETE) { *out = d->val; st = DLSM_OK; }
                matched = 1;
                break;
            }
            c = d->next;
        }
        if (matched) { break; }
        base *b = (base *)c;
        if (past_fence(b, k, kl)) { pid = b->right_sibling; continue; }
        if (b->kind == NODE_BASE_INTERNAL) { pid = route(b, k, kl); continue; }
        int idx = base_find(b, k, kl);
        if (idx >= 0) { *out = b->lents[idx].val; st = DLSM_OK; }
        break;
    }
    dlsm_ebr_exit(&t->ebr, slot);
    return st;
}

/* Merge a leaf chain into a fresh sorted array of live entries (newest wins,
 * tombstones dropped); keys are deep-copied. */
static int lent_cmp(const void *a, const void *b) {
    const leaf_ent *x = a, *y = b;
    return keycmp(x->key, x->klen, y->key, y->klen);
}
typedef struct { const uint8_t *key; uint32_t klen; } seen_ent;
static int seen_has(const seen_ent *s, uint32_t ns, const uint8_t *k, uint32_t kl) {
    for (uint32_t i = 0; i < ns; i++) { if (keycmp(s[i].key, s[i].klen, k, kl) == 0) { return 1; } }
    return 0;
}
static dlsm_status materialize_leaf(chain *head, leaf_ent **out, uint32_t *n) {
    uint32_t nd = 0;
    for (chain *c = head; c->kind == NODE_DELTA; c = ((delta *)c)->next) { nd++; }
    base *b = tail_base(head);
    seen_ent *seen = nd ? malloc(nd * sizeof *seen) : NULL;
    leaf_ent *res = malloc((nd + b->num + 1) * sizeof *res);
    if ((nd && !seen) || !res) { free(seen); free(res); return DLSM_INDEX_E_NOMEM; }
    uint32_t ns = 0, cnt = 0;
    for (chain *c = head; c->kind == NODE_DELTA; c = ((delta *)c)->next) {
        delta *d = (delta *)c;
        if (seen_has(seen, ns, d->key, d->klen)) { continue; }
        seen[ns++] = (seen_ent){ d->key, d->klen };
        if (d->op != OP_DELETE) {
            uint8_t *kc = keydup(d->key, d->klen);
            if (!kc) { goto oom; }
            res[cnt++] = (leaf_ent){ kc, d->klen, d->val };
        }
    }
    for (uint32_t i = 0; i < b->num; i++) {
        if (seen_has(seen, ns, b->lents[i].key, b->lents[i].klen)) { continue; }
        uint8_t *kc = keydup(b->lents[i].key, b->lents[i].klen);
        if (!kc) { goto oom; }
        res[cnt++] = (leaf_ent){ kc, b->lents[i].klen, b->lents[i].val };
    }
    free(seen);
    qsort(res, cnt, sizeof *res, lent_cmp);
    *out = res; *n = cnt;
    return DLSM_OK;
oom:
    for (uint32_t i = 0; i < cnt; i++) { free(res[i].key); }
    free(res); free(seen);
    return DLSM_INDEX_E_NOMEM;
}

/* Collapse pid's leaf chain into one base via CAS; old chain retired to EBR.
 * A per-node try-flag admits only one consolidator at a time (no spin — a second
 * caller just returns), so a hot node is not re-materialized by every writer at
 * once (which would make writes O(chain) and the chain grow without bound). The
 * single consolidator retries a few times to actually win the CAS against the
 * write stream. */
static void try_consolidate(dlsm_index *t, pid_t_ pid) {
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&ent(t, pid)->consolidating, &expected, 1,
                                                 memory_order_acquire, memory_order_relaxed)) {
        return; /* another thread is consolidating this node */
    }
    for (int attempt = 0; attempt < 4; attempt++) {
        chain *h = load_head(t, pid);
        if (h->kind != NODE_DELTA) { break; } /* already a base */
        base *tail = tail_base(h);
        if (tail->kind != NODE_BASE_LEAF) { break; }
        leaf_ent *arr = NULL; uint32_t n = 0;
        if (materialize_leaf(h, &arr, &n) != DLSM_OK) { break; }
        base *nb = base_leaf_new();
        if (!nb) { for (uint32_t i = 0; i < n; i++) { free(arr[i].key); } free(arr); break; }
        nb->lents = arr; nb->num = nb->cap = n;
        nb->low_fence = fdup(tail->low_fence, tail->low_len); nb->low_len = tail->low_len;
        nb->high_fence = fdup(tail->high_fence, tail->high_len); nb->high_len = tail->high_len;
        nb->right_sibling = tail->right_sibling;
        if (atomic_compare_exchange_strong_explicit(&ent(t, pid)->head, &h, (chain *)nb,
                                                     memory_order_acq_rel, memory_order_acquire)) {
            retire_chain(t, h);
            atomic_fetch_add_explicit(&t->consolidations, 1, memory_order_relaxed);
            break;
        }
        free_chain((chain *)nb); /* raced; retry */
    }
    atomic_store_explicit(&ent(t, pid)->consolidating, 0, memory_order_release);
}

/* Descend to the leaf owning `key`, recording the tree path. SMO lock held, so
 * structure is stable (no concurrent split) and no fence hop is expected. */
static pid_t_ descend_with_path(dlsm_index *t, const uint8_t *k, uint32_t kl, pid_t_ *path, int *pl) {
    pid_t_ pid = atomic_load_explicit(&t->root, memory_order_acquire);
    int n = 0;
    for (;;) {
        base *b = tail_base(load_head(t, pid));
        if (past_fence(b, k, kl)) { pid = b->right_sibling; continue; }
        path[n++] = pid;
        if (b->kind == NODE_BASE_INTERNAL) { pid = route(b, k, kl); continue; }
        *pl = n;
        return pid;
    }
}

/* Build a new internal node = P with (sep -> right) inserted just after `child`.
 * Takes ownership of `sep`; deep-copies P's separators. */
static base *clone_internal_insert(const base *P, uint8_t *sep, uint32_t slen, pid_t_ child, pid_t_ right) {
    base *N = base_internal_new(P->level);
    if (!N) { return NULL; }
    N->ients = malloc((P->num + 1) * sizeof *N->ients);
    if (!N->ients) { free(N); return NULL; }
    uint32_t i = 0, w = 0;
    for (; i < P->num; i++) {
        N->ients[w].sep = fdup(P->ients[i].sep, P->ients[i].slen);
        N->ients[w].slen = P->ients[i].slen;
        N->ients[w].child = P->ients[i].child;
        w++;
        if (P->ients[i].child == child) {
            N->ients[w].sep = sep; N->ients[w].slen = slen; N->ients[w].child = right;
            w++;
        }
    }
    N->num = N->cap = w;
    N->low_fence = fdup(P->low_fence, P->low_len); N->low_len = P->low_len;
    N->high_fence = fdup(P->high_fence, P->high_len); N->high_len = P->high_len;
    N->right_sibling = P->right_sibling;
    return N;
}

static void split_internal(dlsm_index *t, pid_t_ *path, int pl, pid_t_ pid);

/* Publish a separator for a new right sibling of `child` (path[pl-1]); grows a
 * new root if `child` was the root. Takes ownership of `sep`. SMO lock held. */
static void install_sep(dlsm_index *t, pid_t_ *path, int pl, pid_t_ child,
                        uint8_t *sep, uint32_t slen, pid_t_ right) {
    if (pl < 2) {
        base *R = base_internal_new((uint8_t)(tail_base(load_head(t, child))->level + 1));
        R->ients = malloc(2 * sizeof *R->ients);
        R->ients[0] = (idx_ent){ NULL, 0, child };
        R->ients[1] = (idx_ent){ sep, slen, right };
        R->num = R->cap = 2;
        pid_t_ rp = alloc_pid(t, (chain *)R);
        atomic_store_explicit(&t->root, rp, memory_order_release);
        return;
    }
    pid_t_ parent = path[pl - 2];
    chain *oldh = load_head(t, parent);
    base *Pnew = clone_internal_insert((base *)oldh, sep, slen, child, right);
    atomic_store_explicit(&ent(t, parent)->head, (chain *)Pnew, memory_order_release);
    retire_chain(t, oldh);
    if (Pnew->num > DLSM_INTERNAL_MAX) { split_internal(t, path, pl - 1, parent); }
}

static void split_internal(dlsm_index *t, pid_t_ *path, int pl, pid_t_ pid) {
    base *N = (base *)load_head(t, pid);
    uint32_t mid = N->num / 2;
    uint8_t *promoted = keydup(N->ients[mid].sep, N->ients[mid].slen);
    uint32_t plen = N->ients[mid].slen;
    uint32_t rn = N->num - mid;

    base *NR = base_internal_new(N->level);
    NR->ients = malloc(rn * sizeof *NR->ients);
    NR->ients[0] = (idx_ent){ NULL, 0, N->ients[mid].child };
    for (uint32_t j = mid + 1; j < N->num; j++) {
        NR->ients[j - mid].sep = fdup(N->ients[j].sep, N->ients[j].slen);
        NR->ients[j - mid].slen = N->ients[j].slen;
        NR->ients[j - mid].child = N->ients[j].child;
    }
    NR->num = NR->cap = rn;
    NR->low_fence = keydup(promoted, plen); NR->low_len = plen;
    NR->high_fence = fdup(N->high_fence, N->high_len); NR->high_len = N->high_len;
    NR->right_sibling = N->right_sibling;
    pid_t_ nrp = alloc_pid(t, (chain *)NR);

    base *NL = base_internal_new(N->level);
    NL->ients = malloc(mid * sizeof *NL->ients);
    for (uint32_t j = 0; j < mid; j++) {
        NL->ients[j].sep = (j == 0) ? NULL : fdup(N->ients[j].sep, N->ients[j].slen);
        NL->ients[j].slen = (j == 0) ? 0 : N->ients[j].slen;
        NL->ients[j].child = N->ients[j].child;
    }
    NL->num = NL->cap = mid;
    NL->low_fence = fdup(N->low_fence, N->low_len); NL->low_len = N->low_len;
    NL->high_fence = keydup(promoted, plen); NL->high_len = plen;
    NL->right_sibling = nrp;

    atomic_store_explicit(&ent(t, pid)->head, (chain *)NL, memory_order_release);
    retire_chain(t, (chain *)N);
    atomic_fetch_add_explicit(&t->internal_splits, 1, memory_order_relaxed);
    install_sep(t, path, pl, pid, promoted, plen, nrp);
}

/* Split an over-full leaf; the right half's first key is the separator pushed
 * up. Re-validates under the SMO lock (a racing writer may have changed things).*/
static void split_leaf(dlsm_index *t, const uint8_t *k, uint32_t kl) {
    /* Opportunistic: only one structural modifier at a time, and contenders skip
     * rather than spin (a later op will split this leaf). Splits are never
     * required for correctness — point ops work at any node size. */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&t->smo_busy, &expected, 1,
                                                 memory_order_acquire, memory_order_relaxed)) {
        return;
    }
    pid_t_ path[DLSM_MAX_DEPTH];
    int pl = 0;
    pid_t_ pid = descend_with_path(t, k, kl, path, &pl);
    for (int attempt = 0; attempt < 8; attempt++) {
        chain *h = load_head(t, pid);
        leaf_ent *arr = NULL; uint32_t n = 0;
        if (materialize_leaf(h, &arr, &n) != DLSM_OK) { break; }
        if (n <= DLSM_LEAF_MAX) { for (uint32_t i = 0; i < n; i++) { free(arr[i].key); } free(arr); break; }
        base *tail = tail_base(h);
        uint32_t mid = n / 2;
        uint32_t sep_len = arr[mid].klen;

        base *L = base_leaf_new();
        base *R = base_leaf_new();
        L->lents = malloc(mid * sizeof *L->lents);
        R->lents = malloc((n - mid) * sizeof *R->lents);
        uint8_t *sep = keydup(arr[mid].key, sep_len);
        if (!L || !R || !L->lents || !R->lents || !sep) {
            free(L->lents); free(R->lents); free(L); free(R); free(sep);
            for (uint32_t i = 0; i < n; i++) { free(arr[i].key); } free(arr);
            break;
        }
        memcpy(L->lents, arr, mid * sizeof *arr);             /* moves key ownership */
        memcpy(R->lents, arr + mid, (n - mid) * sizeof *arr);
        free(arr);
        L->num = L->cap = mid; R->num = R->cap = n - mid;
        L->low_fence = fdup(tail->low_fence, tail->low_len); L->low_len = tail->low_len;
        L->high_fence = keydup(sep, sep_len); L->high_len = sep_len;
        R->low_fence = keydup(sep, sep_len); R->low_len = sep_len;
        R->high_fence = fdup(tail->high_fence, tail->high_len); R->high_len = tail->high_len;

        pid_t_ rp = alloc_pid(t, (chain *)R);
        L->right_sibling = rp;
        R->right_sibling = tail->right_sibling;

        if (atomic_compare_exchange_strong_explicit(&ent(t, pid)->head, &h, (chain *)L,
                                                     memory_order_acq_rel, memory_order_acquire)) {
            retire_chain(t, h);
            atomic_fetch_add_explicit(&t->leaf_splits, 1, memory_order_relaxed);
            install_sep(t, path, pl, pid, sep, sep_len, rp);
            break;
        }
        /* a writer raced in; undo and retry (re-materialize includes its delta) */
        atomic_store_explicit(&ent(t, rp)->head, NULL, memory_order_relaxed);
        atomic_fetch_sub_explicit(&t->next_pid, 1, memory_order_relaxed);
        free_chain((chain *)L);
        free_chain((chain *)R);
        free(sep);   /* only install_sep (success path) consumes sep */
    }
    atomic_store_explicit(&t->smo_busy, 0, memory_order_release);
}

static dlsm_status write_op(dlsm_index *t, delta_op op, const void *key, size_t klen, dlsm_delta_pointer v) {
    if (!t || (!key && klen)) { return DLSM_INDEX_E_INVAL; }
    const uint8_t *k = key;
    uint32_t kl = (uint32_t)klen;
    int slot = ebr_slot(&t->ebr);
    dlsm_ebr_enter(&t->ebr, slot);
    dlsm_status st = DLSM_OK;

    /* find the leaf */
    pid_t_ pid = atomic_load_explicit(&t->root, memory_order_acquire);
    for (;;) {
        base *b = tail_base(load_head(t, pid));
        if (past_fence(b, k, kl)) { pid = b->right_sibling; continue; }
        if (b->kind == NODE_BASE_INTERNAL) { pid = route(b, k, kl); continue; }
        break;
    }

    /* CAS-install a delta onto the leaf head; retry on contention. */
    uint32_t depth = 0;
    for (;;) {
        chain *h = load_head(t, pid);
        base *b = tail_base(h);
        if (past_fence(b, k, kl)) { pid = b->right_sibling; continue; } /* split moved us */
        uint32_t cur = (h->kind == NODE_DELTA) ? ((delta *)h)->depth : 0;
        if (cur >= DLSM_CHAIN_CAP) {
            /* Chain is at its cap under contention: drain it instead of appending,
             * so the (single) consolidator can win its CAS and collapse it. This
             * bounds materialize work at O(cap) and prevents an O(n^2) blowup. */
            try_consolidate(t, pid);
            sched_yield();
            continue;
        }
        depth = cur + 1;
        delta *d = calloc(1, sizeof *d);
        uint8_t *kc = keydup(k, kl);
        if (!d || !kc) { free(d); free(kc); st = DLSM_INDEX_E_NOMEM; goto out; }
        d->kind = NODE_DELTA; d->op = op; d->key = kc; d->klen = kl; d->val = v;
        d->next = h; d->depth = depth;
        if (atomic_compare_exchange_strong_explicit(&ent(t, pid)->head, &h, (chain *)d,
                                                     memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
        free(kc); free(d); /* lost the race, retry */
    }

    if (depth >= DLSM_CONSOLIDATE_DEPTH) { try_consolidate(t, pid); }
    {
        base *b = tail_base(load_head(t, pid));
        if (b->kind == NODE_BASE_LEAF && b->num > DLSM_LEAF_MAX) { split_leaf(t, k, kl); }
    }
out:
    dlsm_ebr_exit(&t->ebr, slot);
    if ((atomic_fetch_add_explicit(&t->gc_tick, 1, memory_order_relaxed) & 0xFF) == 0) {
        dlsm_ebr_try_advance(&t->ebr);
    }
    return st;
}

dlsm_status dlsm_index_insert(dlsm_index *t, const void *key, size_t klen, dlsm_delta_pointer v) {
    return write_op(t, OP_INSERT, key, klen, v);
}
dlsm_status dlsm_index_update(dlsm_index *t, const void *key, size_t klen, dlsm_delta_pointer v) {
    return write_op(t, OP_UPDATE, key, klen, v);
}
dlsm_status dlsm_index_delete(dlsm_index *t, const void *key, size_t klen) {
    dlsm_delta_pointer tomb = { .kind = DLSM_DP_TOMBSTONE };
    return write_op(t, OP_DELETE, key, klen, tomb);
}

/* ---- stats (single-threaded: called after writers quiesce) ---- */
static uint8_t node_level(chain *h) { return tail_base(h)->level; }
static uint64_t count_live(dlsm_index *t, pid_t_ pid) {
    chain *h = load_head(t, pid);
    base *b = tail_base(h);
    if (b->kind == NODE_BASE_INTERNAL) {
        uint64_t s = 0;
        for (uint32_t i = 0; i < b->num; i++) { s += count_live(t, b->ients[i].child); }
        return s;
    }
    leaf_ent *arr = NULL; uint32_t n = 0;
    if (materialize_leaf(h, &arr, &n) != DLSM_OK) { return 0; }
    for (uint32_t i = 0; i < n; i++) { free(arr[i].key); }
    free(arr);
    return n;
}
void dlsm_index_stats_get(const dlsm_index *ct, dlsm_index_stats *out) {
    if (!out) { return; }
    *out = (dlsm_index_stats){0};
    if (!ct) { return; }
    dlsm_index *t = (dlsm_index *)ct;
    pid_t_ root = atomic_load_explicit(&t->root, memory_order_acquire);
    out->height = node_level(load_head(t, root)) + 1u;
    out->consolidations = atomic_load_explicit(&t->consolidations, memory_order_relaxed);
    out->leaf_splits = atomic_load_explicit(&t->leaf_splits, memory_order_relaxed);
    out->internal_splits = atomic_load_explicit(&t->internal_splits, memory_order_relaxed);
    out->live_keys = count_live(t, root);
}
