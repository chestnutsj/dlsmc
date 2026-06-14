#ifndef DLSM_CORE_H
#define DLSM_CORE_H

#include <stdint.h>

/* Stable numeric status. 0 == OK. Error bands (see architecture.md §8):
 *   10000+ dlsm-shm, 20000+ greenthread, 30000+ sync, 40000+ core. */
typedef int32_t dlsm_status;

enum { DLSM_OK = 0 };

/* Single source of truth for status messages (architecture.md §8). Strings are
 * macro-defined so a future i18n layer can swap the catalog without touching
 * call sites. Common messages live here; per-library lists live in their own
 * headers as X-macro tables (e.g. DLSM_SHM_ERROR_LIST). */
#define DLSM_MSG_OK      "ok"
#define DLSM_MSG_UNKNOWN "unknown error"
#define DLSM_MSG_INVAL   "invalid argument"

/* English message for a core-band/OK status; DLSM_MSG_UNKNOWN otherwise.
 * Per-library codes have their own *_strerror (e.g. dlsm_shm_strerror). */
const char *dlsm_strerror(dlsm_status st);

#endif /* DLSM_CORE_H */
