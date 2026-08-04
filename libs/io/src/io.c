#define _GNU_SOURCE
#include "dlsm/io.h"
#include "dlsm/greenthread.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/io_uring.h>

/* io_uring's submit->completion ordering is a real happens-before edge that
 * ThreadSanitizer cannot see (the kernel syscalls are opaque). Annotate it so
 * TSAN does not report false races on the per-token slot: the submitter
 * releases the slot before submitting; the reaper acquires it after the CQE. */
#if defined(__SANITIZE_THREAD__)
#  define DLSM_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define DLSM_TSAN 1
#  endif
#endif
#ifdef DLSM_TSAN
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);
#  define TSAN_RELEASE(p) __tsan_release(p)
#  define TSAN_ACQUIRE(p) __tsan_acquire(p)
#else
#  define TSAN_RELEASE(p) ((void)(p))
#  define TSAN_ACQUIRE(p) ((void)(p))
#endif

#define DLSM_IO_STOP_TOKEN UINT64_MAX

static int iou_setup(unsigned entries, struct io_uring_params *p) {
    return (int)syscall(__NR_io_uring_setup, entries, p);
}
static int iou_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags) {
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags,
                        (void *)0, (size_t)0);
}

struct slot {
    _Atomic int   res;
    _Atomic int   wake_done;
    dlsm_gt_task *task;
    int           next; /* free-list link, -1 = none */
};

struct dlsm_io {
    int fd;

    /* SQ ring */
    void     *sq_map;  size_t sq_map_len;
    void     *sqe_map; size_t sqe_map_len;
    unsigned *s_head, *s_tail, *s_mask, *s_array;
    struct io_uring_sqe *sqes;

    /* CQ ring */
    void     *cq_map;  size_t cq_map_len; /* MAP_FAILED-sentinel unused when single mmap */
    int       cq_single_mmap;
    unsigned *c_head, *c_tail, *c_mask;
    struct io_uring_cqe *cqes;

    /* completion slots / submission serialization */
    struct slot    *slots;
    int             nslots;
    int             free_head;
    pthread_mutex_t mtx;

    /* reaper */
    pthread_t       reaper;
    _Atomic int     stop;
    int             mtx_inited;
    int             reaper_started;
};

const char *dlsm_io_strerror(dlsm_status st) {
    switch (st) {
#define DLSM_IO_MSG_X(name, code, msg) case name: return msg;
    DLSM_IO_ERROR_LIST(DLSM_IO_MSG_X)
#undef DLSM_IO_MSG_X
    default: return DLSM_MSG_UNKNOWN;
    }
}

/* ---- slot pool (under mtx) ------------------------------------------------ */
static int slot_alloc(dlsm_io *io) {
    pthread_mutex_lock(&io->mtx);
    int i = io->free_head;
    if (i >= 0) { io->free_head = io->slots[i].next; }
    pthread_mutex_unlock(&io->mtx);
    return i;
}
static void slot_free(dlsm_io *io, int i) {
    pthread_mutex_lock(&io->mtx);
    io->slots[i].next = io->free_head;
    io->free_head = i;
    pthread_mutex_unlock(&io->mtx);
}

/* ---- submission (serializes the SQ tail under mtx) ------------------------ */
static void submit(dlsm_io *io, uint8_t op, int fd, uint64_t addr, unsigned len,
                   uint64_t off, uint32_t fsync_flags, uint64_t user_data) {
    pthread_mutex_lock(&io->mtx);
    unsigned tail = *io->s_tail;
    unsigned idx = tail & *io->s_mask;
    struct io_uring_sqe *sqe = &io->sqes[idx];
    memset(sqe, 0, sizeof *sqe);
    sqe->opcode = op;
    sqe->fd = fd;
    sqe->addr = addr;
    sqe->len = len;
    sqe->off = off;
    sqe->fsync_flags = fsync_flags; /* union; harmless for non-fsync ops */
    sqe->user_data = user_data;
    io->s_array[idx] = idx;
    atomic_store_explicit((_Atomic unsigned *)io->s_tail, tail + 1, memory_order_release);
    (void)iou_enter(io->fd, 1, 0, 0); /* submit, do not wait */
    pthread_mutex_unlock(&io->mtx);
}

/* ---- reaper thread -------------------------------------------------------- */
static void *reaper_main(void *arg) {
    dlsm_io *io = (dlsm_io *)arg;
    for (;;) {
        (void)iou_enter(io->fd, 0, 1, IORING_ENTER_GETEVENTS); /* wait >= 1 CQE */
        unsigned head = *io->c_head;
        unsigned tail = atomic_load_explicit((_Atomic unsigned *)io->c_tail,
                                             memory_order_acquire);
        while (head != tail) {
            struct io_uring_cqe *cqe = &io->cqes[head & *io->c_mask];
            uint64_t ud = cqe->user_data;
            int res = cqe->res;
            head++;
            if (ud == DLSM_IO_STOP_TOKEN) { continue; }
            struct slot *s = &io->slots[(int)ud];
            TSAN_ACQUIRE(s); /* pairs with the submitter's release (io_uring edge) */
            atomic_store_explicit(&s->res, res, memory_order_release);
            dlsm_gt_unpark(s->task);
            /* The resumed GT may be the runtime's final task. Do not let it
             * return from do_op and free the runtime while this reaper still
             * has an unpark call using the task/VP objects on its stack. */
            atomic_store_explicit(&s->wake_done, 1, memory_order_release);
        }
        atomic_store_explicit((_Atomic unsigned *)io->c_head, head, memory_order_release);
        if (atomic_load_explicit(&io->stop, memory_order_acquire)) { break; }
    }
    return NULL;
}

/* ---- one positioned op: submit -> park -> result -------------------------- */
static ssize_t do_op(dlsm_io *io, uint8_t op, int fd, uint64_t addr, unsigned len,
                     uint64_t off, uint32_t fsync_flags) {
    if (!dlsm_gt_self()) { errno = ENOTSUP; return -1; }
    int tok;
    while ((tok = slot_alloc(io)) < 0) { dlsm_gt_yield(); } /* cooperative backpressure */
    io->slots[tok].task = dlsm_gt_self();
    atomic_store_explicit(&io->slots[tok].wake_done, 0, memory_order_relaxed);
    TSAN_RELEASE(&io->slots[tok]); /* publish slot.task before the kernel sees the SQE */
    submit(io, op, fd, addr, len, off, fsync_flags, (uint64_t)tok);
    dlsm_gt_park();
    while (!atomic_load_explicit(&io->slots[tok].wake_done,
                                 memory_order_acquire)) {
        dlsm_gt_yield();
    }
    int res = atomic_load_explicit(&io->slots[tok].res, memory_order_acquire);
    slot_free(io, tok);
    if (res < 0) { errno = -res; return -1; }
    return res;
}

ssize_t dlsm_io_read_at(dlsm_io *io, int fd, void *buf, size_t len, off_t off) {
    return do_op(io, IORING_OP_READ, fd, (uint64_t)(uintptr_t)buf, (unsigned)len,
                 (uint64_t)off, 0);
}
ssize_t dlsm_io_write_at(dlsm_io *io, int fd, const void *buf, size_t len, off_t off) {
    return do_op(io, IORING_OP_WRITE, fd, (uint64_t)(uintptr_t)buf, (unsigned)len,
                 (uint64_t)off, 0);
}
int dlsm_io_fsync(dlsm_io *io, int fd) {
    return (int)do_op(io, IORING_OP_FSYNC, fd, 0, 0, 0, 0);
}
int dlsm_io_fdatasync(dlsm_io *io, int fd) {
    return (int)do_op(io, IORING_OP_FSYNC, fd, 0, 0, 0, IORING_FSYNC_DATASYNC);
}

int dlsm_io_accept(dlsm_io *io, int listen_fd, struct sockaddr *addr, socklen_t *addrlen) {
    /* io_uring ACCEPT: addr -> sockaddr*, off/addr2 -> socklen_t* */
    return (int)do_op(io, IORING_OP_ACCEPT, listen_fd, (uint64_t)(uintptr_t)addr, 0,
                      (uint64_t)(uintptr_t)addrlen, 0);
}
ssize_t dlsm_io_recv(dlsm_io *io, int fd, void *buf, size_t len) {
    return do_op(io, IORING_OP_RECV, fd, (uint64_t)(uintptr_t)buf, (unsigned)len, 0, 0);
}
ssize_t dlsm_io_send(dlsm_io *io, int fd, const void *buf, size_t len) {
    return do_op(io, IORING_OP_SEND, fd, (uint64_t)(uintptr_t)buf, (unsigned)len, 0, 0);
}

/* ---- setup / teardown ----------------------------------------------------- */
dlsm_io *dlsm_io_new(unsigned entries) {
    if (entries == 0) { entries = 64; }
    dlsm_io *io = calloc(1, sizeof *io);
    if (!io) { return NULL; }
    io->fd = -1;

    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    io->fd = iou_setup(entries, &p);
    if (io->fd < 0) { free(io); return NULL; }

    size_t sqsz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cqsz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    io->cq_single_mmap = (p.features & IORING_FEAT_SINGLE_MMAP) ? 1 : 0;
    if (io->cq_single_mmap && cqsz > sqsz) { sqsz = cqsz; }

    io->sq_map_len = sqsz;
    io->sq_map = mmap(0, sqsz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                      io->fd, IORING_OFF_SQ_RING);
    void *cq = io->sq_map;
    if (io->sq_map != MAP_FAILED && !io->cq_single_mmap) {
        io->cq_map_len = cqsz;
        io->cq_map = mmap(0, cqsz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                          io->fd, IORING_OFF_CQ_RING);
        cq = io->cq_map;
    }
    io->sqe_map_len = p.sq_entries * sizeof(struct io_uring_sqe);
    io->sqe_map = mmap(0, io->sqe_map_len, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, io->fd, IORING_OFF_SQES);
    if (io->sq_map == MAP_FAILED || cq == MAP_FAILED || io->sqe_map == MAP_FAILED) {
        dlsm_io_free(io);
        return NULL;
    }

    io->s_head  = (unsigned *)((char *)io->sq_map + p.sq_off.head);
    io->s_tail  = (unsigned *)((char *)io->sq_map + p.sq_off.tail);
    io->s_mask  = (unsigned *)((char *)io->sq_map + p.sq_off.ring_mask);
    io->s_array = (unsigned *)((char *)io->sq_map + p.sq_off.array);
    io->sqes    = (struct io_uring_sqe *)io->sqe_map;
    io->c_head  = (unsigned *)((char *)cq + p.cq_off.head);
    io->c_tail  = (unsigned *)((char *)cq + p.cq_off.tail);
    io->c_mask  = (unsigned *)((char *)cq + p.cq_off.ring_mask);
    io->cqes    = (struct io_uring_cqe *)((char *)cq + p.cq_off.cqes);

    io->nslots = (int)p.sq_entries;
    io->slots = calloc((size_t)io->nslots, sizeof(struct slot));
    if (!io->slots) { dlsm_io_free(io); return NULL; }
    for (int i = 0; i < io->nslots; i++) { io->slots[i].next = i + 1; }
    io->slots[io->nslots - 1].next = -1;
    io->free_head = 0;

    pthread_mutex_init(&io->mtx, NULL);
    io->mtx_inited = 1;
    atomic_store(&io->stop, 0);
    if (pthread_create(&io->reaper, NULL, reaper_main, io) != 0) {
        dlsm_io_free(io);
        return NULL;
    }
    io->reaper_started = 1;
    return io;
}

void dlsm_io_free(dlsm_io *io) {
    if (!io) { return; }
    if (io->reaper_started) {
        /* stop the reaper: flag + wake it with a NOP completion */
        atomic_store_explicit(&io->stop, 1, memory_order_release);
        submit(io, IORING_OP_NOP, -1, 0, 0, 0, 0, DLSM_IO_STOP_TOKEN);
        pthread_join(io->reaper, NULL);
    }
    if (io->mtx_inited) { pthread_mutex_destroy(&io->mtx); }
    free(io->slots);
    if (io->sqe_map && io->sqe_map != MAP_FAILED) { munmap(io->sqe_map, io->sqe_map_len); }
    if (io->cq_map && io->cq_map != MAP_FAILED) { munmap(io->cq_map, io->cq_map_len); }
    if (io->sq_map && io->sq_map != MAP_FAILED) { munmap(io->sq_map, io->sq_map_len); }
    if (io->fd >= 0) { close(io->fd); }
    free(io);
}
