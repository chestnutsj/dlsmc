#ifndef DLSM_IO_H
#define DLSM_IO_H

#include "dlsm/core.h"
#include <stddef.h>
#include <sys/types.h>  /* ssize_t, off_t */
#include <sys/socket.h> /* struct sockaddr, socklen_t */

/* io error band: 50000+ (extends architecture.md §8 for the dlsm-io layer).
 * Single source of truth: (name, code, message) — i18n-ready X-macro. */
#define DLSM_IO_ERROR_LIST(X)                                       \
    X(DLSM_IO_E_SETUP,  50001, "io_uring_setup failed")             \
    X(DLSM_IO_E_MMAP,   50002, "io_uring ring mmap failed")         \
    X(DLSM_IO_E_NOTASK, 50003, "I/O issued outside a green thread") \
    X(DLSM_IO_E_INVAL,  50004, DLSM_MSG_INVAL)

enum {
#define DLSM_IO_ENUM_X(name, code, msg) name = code,
    DLSM_IO_ERROR_LIST(DLSM_IO_ENUM_X)
#undef DLSM_IO_ENUM_X
};

const char *dlsm_io_strerror(dlsm_status st);

typedef struct dlsm_io dlsm_io;

/* Create an io_uring-backed async I/O context with `entries` ring depth
 * (rounded up to a power of two by the kernel; 0 => 64). Spawns a reaper
 * thread that wakes parked green threads on completion. Returns NULL on
 * failure (e.g. io_uring unavailable). */
dlsm_io *dlsm_io_new(unsigned entries);
void     dlsm_io_free(dlsm_io *io);

/* Blocking-style positioned I/O, callable only from within a running green
 * thread: internally submit an SQE, park, and resume when the CQE arrives.
 * read/write return the byte count (>= 0) or -1 with errno set; fsync return
 * 0 or -1 with errno set. Outside a green thread: -1 / errno = ENOTSUP. */
ssize_t dlsm_io_read_at (dlsm_io *io, int fd, void *buf, size_t len, off_t off);
ssize_t dlsm_io_write_at(dlsm_io *io, int fd, const void *buf, size_t len, off_t off);
int     dlsm_io_fsync    (dlsm_io *io, int fd);
int     dlsm_io_fdatasync(dlsm_io *io, int fd);

/* Socket I/O (same blocking-style/park semantics; green-thread context only).
 * accept returns a new connection fd (>= 0); recv/send return byte counts
 * (recv == 0 means the peer closed); all return -1 with errno on error. */
int     dlsm_io_accept(dlsm_io *io, int listen_fd, struct sockaddr *addr, socklen_t *addrlen);
ssize_t dlsm_io_recv  (dlsm_io *io, int fd, void *buf, size_t len);
ssize_t dlsm_io_send  (dlsm_io *io, int fd, const void *buf, size_t len);

#endif /* DLSM_IO_H */
