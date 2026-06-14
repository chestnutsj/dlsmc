#ifndef DLSM_SHM_H
#define DLSM_SHM_H

#include "dlsm/core.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef DLSM_SHM_BASE_ADDR
#define DLSM_SHM_BASE_ADDR 0x200000000000ULL /* 32 TiB */
#endif

/* shm error band: 10000+ (architecture.md §8). Single source of truth:
 * (name, code, message) — generates both the enum and dlsm_shm_strerror.
 * Messages are macros so a future i18n layer can swap the catalog. */
#define DLSM_SHM_ERROR_LIST(X)                                        \
    X(DLSM_SHM_E_OPEN,          10001, "shm_open failed")             \
    X(DLSM_SHM_E_FTRUNCATE,     10002, "ftruncate failed")           \
    X(DLSM_SHM_E_BASE_OCCUPIED, 10003, "fixed base address occupied") \
    X(DLSM_SHM_E_IN_USE,        10004, "segment in use by a live owner") \
    X(DLSM_SHM_E_BAD_MAGIC,     10005, "bad segment magic")          \
    X(DLSM_SHM_E_BAD_VERSION,   10006, "unsupported segment version") \
    X(DLSM_SHM_E_OOM,           10007, "arena exhausted")            \
    X(DLSM_SHM_E_INVAL,         10008, DLSM_MSG_INVAL)                \
    X(DLSM_SHM_E_NOMEM,         10009, "out of memory")

enum {
#define DLSM_SHM_ENUM_X(name, code, msg) name = code,
    DLSM_SHM_ERROR_LIST(DLSM_SHM_ENUM_X)
#undef DLSM_SHM_ENUM_X
};

typedef struct dlsm_shm dlsm_shm; /* opaque */
typedef uint64_t dlsm_shm_offset;

/* Lifecycle (single-writer host). */
dlsm_status dlsm_shm_create_or_recover(const char *name, size_t size, dlsm_shm **out);
dlsm_status dlsm_shm_attach_readonly(const char *name, dlsm_shm **out);
void        dlsm_shm_detach(dlsm_shm *s);
dlsm_status dlsm_shm_cleanup_if_stale(const char *name, bool *cleaned);

/* Allocation: CAS bump, multi-thread safe, returns an absolute pointer
 * inside the segment, aligned to `align` (power of two). Returns NULL on
 * exhaustion / invalid args / read-only handle. Never frees a single object. */
void *dlsm_shm_alloc(dlsm_shm *s, size_t size, size_t align);

/* Introspection. */
void  *dlsm_shm_base(const dlsm_shm *s);
size_t dlsm_shm_used(const dlsm_shm *s);
size_t dlsm_shm_capacity(const dlsm_shm *s);

/* Stable segment references. Stored shared structures should retain offsets,
 * not process pointers; pointer conversion is confined to the API boundary. */
bool  dlsm_shm_offset_of(const dlsm_shm *s, const void *ptr, dlsm_shm_offset *out);
void *dlsm_shm_pointer(const dlsm_shm *s, dlsm_shm_offset offset, size_t size);

/* English message for an shm-band status. */
const char *dlsm_shm_strerror(dlsm_status st);

#endif /* DLSM_SHM_H */
