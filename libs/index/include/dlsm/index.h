#ifndef DLSM_INDEX_H
#define DLSM_INDEX_H

#include "dlsm/core.h"
#include <stddef.h>
#include <stdint.h>

/* index error band: 60000+ (extends architecture.md §8 for the dlsm-index /
 * Bw-Tree layer). Single source of truth: (name, code, message) — generates
 * both the enum and dlsm_index_strerror. Messages are macros so a future i18n
 * layer can swap the catalog without touching call sites. */
#define DLSM_INDEX_ERROR_LIST(X)                              \
    X(DLSM_INDEX_E_NOMEM,    60001, "out of memory")          \
    X(DLSM_INDEX_E_NOTFOUND, 60002, "key not found")          \
    X(DLSM_INDEX_E_INVAL,    60003, DLSM_MSG_INVAL)

enum {
#define DLSM_INDEX_ENUM_X(name, code, msg) name = code,
    DLSM_INDEX_ERROR_LIST(DLSM_INDEX_ENUM_X)
#undef DLSM_INDEX_ENUM_X
};

const char *dlsm_index_strerror(dlsm_status st);

/* DeltaPointer — 16-byte polymorphic value pointer (bwtree.md §2.3). The index
 * stores Key -> DeltaPointer and never interprets the value bytes: HOT points
 * into the Delta-Log file (file_id + byte offset), COLD_VORTEX into a columnar
 * file, TOMBSTONE marks a deletion. P1 uses a flat 16B layout (the spec's union
 * variants all fit in file_id + offset); richer COLD addressing lands with the
 * Vortex integration (P4). */
enum { DLSM_DP_HOT = 0, DLSM_DP_COLD_VORTEX = 1, DLSM_DP_TOMBSTONE = 2 };

typedef struct {
    uint8_t  kind;      /* DLSM_DP_* */
    uint8_t  _pad[3];
    uint32_t file_id;   /* HOT: Delta-Log file id; COLD: Vortex file id */
    uint64_t offset;    /* HOT: byte offset; COLD: row id; TOMBSTONE: deleted_ts */
} dlsm_delta_pointer;

_Static_assert(sizeof(dlsm_delta_pointer) == 16, "delta_pointer must be 16B");

/* A single-threaded in-memory Bw-Tree (ROADMAP 主线 B P1): Mapping Table +
 * Base/Delta nodes, get/insert/update/delete, depth-triggered consolidate, and
 * eager split with recursive root growth. Keys are opaque byte strings ordered
 * by memcmp (the memcomparable contract: lexicographic byte order == value
 * order, so the index needs no codec). Not yet thread-safe — concurrent callers
 * must serialize (P2 adds CAS install + epoch GC). */
typedef struct dlsm_index dlsm_index;

dlsm_index *dlsm_index_new(void);
void        dlsm_index_free(dlsm_index *t);

/* insert/update install the latest value for `key` (both upsert at the index
 * level; the op distinction drives delta typing / secondary-index logic later).
 * delete installs a tombstone. All return DLSM_OK, or DLSM_INDEX_E_NOMEM /
 * DLSM_INDEX_E_INVAL. */
dlsm_status dlsm_index_insert(dlsm_index *t, const void *key, size_t klen, dlsm_delta_pointer v);
dlsm_status dlsm_index_update(dlsm_index *t, const void *key, size_t klen, dlsm_delta_pointer v);
dlsm_status dlsm_index_delete(dlsm_index *t, const void *key, size_t klen);

/* Fetch the visible pointer for `key`. Returns DLSM_OK and fills *out on hit;
 * DLSM_INDEX_E_NOTFOUND if absent or tombstoned. */
dlsm_status dlsm_index_get(const dlsm_index *t, const void *key, size_t klen, dlsm_delta_pointer *out);

/* Introspection for tests / observability (dlsm-stat style). */
typedef struct {
    uint32_t height;          /* number of levels (1 = a single leaf) */
    uint64_t live_keys;       /* keys currently visible (tombstones excluded) */
    uint64_t consolidations;  /* delta chains collapsed into a fresh base node */
    uint64_t leaf_splits;     /* eager leaf splits */
    uint64_t internal_splits; /* eager internal-node splits (root grows on top) */
} dlsm_index_stats;

void dlsm_index_stats_get(const dlsm_index *t, dlsm_index_stats *out);

#endif /* DLSM_INDEX_H */
