#define _GNU_SOURCE
#include "dlsm/shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define DLSM_SHM_MAGIC   0x4D4853204D534C44ULL /* "DLSM SHM" little-endian-ish */
#define DLSM_SHM_VERSION 2u
struct ShmHeader {
    _Atomic uint64_t magic;
    _Atomic uint32_t version;
    uint32_t _pad;
    _Atomic uint64_t base_addr;
    _Atomic uint64_t total_size;
    _Atomic uint64_t bump;
    _Atomic int32_t  owner_pid;
};

struct dlsm_shm {
    struct ShmHeader *hdr; /* == mapped base */
    size_t mapped_size;
    int    fd;
    bool   readonly;
};

static size_t align_up(size_t v, size_t a) { return (v + (a - 1)) & ~(a - 1); }

static void init_header(struct ShmHeader *h, size_t size) {
    atomic_store_explicit(&h->magic, 0, memory_order_relaxed);
    atomic_store_explicit(&h->version, DLSM_SHM_VERSION, memory_order_relaxed);
    h->_pad       = 0;
    atomic_store_explicit(&h->base_addr, (uint64_t)(uintptr_t)h, memory_order_relaxed);
    atomic_store_explicit(&h->total_size, (uint64_t)size, memory_order_relaxed);
    atomic_store_explicit(&h->bump,
        align_up(sizeof(struct ShmHeader), 16), memory_order_relaxed);
    atomic_store_explicit(&h->owner_pid, (int32_t)getpid(), memory_order_relaxed);
    atomic_store_explicit(&h->magic, DLSM_SHM_MAGIC, memory_order_release);
}

static void *map_rw_at_base(int fd, size_t size) {
    void *p = mmap((void *)(uintptr_t)DLSM_SHM_BASE_ADDR, size,
                   PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    if (p == (void *)(uintptr_t)DLSM_SHM_BASE_ADDR) { return p; }
    if (p != MAP_FAILED) { munmap(p, size); }
    return MAP_FAILED;
}

static int writer_lock(int fd) {
    struct flock lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0
    };
    if (fcntl(fd, F_OFD_SETLK, &lock) == 0) { return 1; }
    if (errno == EACCES || errno == EAGAIN) { return 0; }
    return -1;
}

const char *dlsm_shm_strerror(dlsm_status st) {
    switch (st) {
#define DLSM_SHM_MSG_X(name, code, msg) case name: return msg;
    DLSM_SHM_ERROR_LIST(DLSM_SHM_MSG_X)
#undef DLSM_SHM_MSG_X
    default: return DLSM_MSG_UNKNOWN;
    }
}

/* Map the header read-only to learn base/size/owner. Returns 0 on success and
 * fills out_owner and out_size (and out_base if non-NULL); sets *bad to an shm
 * status on magic or version error. */
static int read_header(int fd, int32_t *out_owner, uint64_t *out_size,
                       uint64_t *out_base, dlsm_status *bad) {
    struct stat st;
    if (fstat(fd, &st) != 0) { *bad = DLSM_SHM_E_OPEN; return -1; }
    if (st.st_size < (off_t)sizeof(struct ShmHeader)) {
        *bad = DLSM_SHM_E_BAD_MAGIC;
        return -1;
    }
    void *t = mmap(NULL, sizeof(struct ShmHeader), PROT_READ, MAP_SHARED, fd, 0);
    if (t == MAP_FAILED) { *bad = DLSM_SHM_E_OPEN; return -1; }
    struct ShmHeader *h = (struct ShmHeader *)t;
    uint64_t magic = atomic_load_explicit(&h->magic, memory_order_acquire);
    uint32_t version = atomic_load_explicit(&h->version, memory_order_relaxed);
    uint64_t size = atomic_load_explicit(&h->total_size, memory_order_relaxed);
    uint64_t base = atomic_load_explicit(&h->base_addr, memory_order_relaxed);
    int32_t owner = atomic_load_explicit(&h->owner_pid, memory_order_relaxed);
    uint64_t verify = atomic_load_explicit(&h->magic, memory_order_acquire);
    munmap(t, sizeof(struct ShmHeader));
    if (magic != DLSM_SHM_MAGIC || verify != magic) { *bad = DLSM_SHM_E_BAD_MAGIC; return -1; }
    if (version != DLSM_SHM_VERSION) { *bad = DLSM_SHM_E_BAD_VERSION; return -1; }
    if (size != (uint64_t)st.st_size || size < sizeof(struct ShmHeader) + 16 ||
        base != DLSM_SHM_BASE_ADDR || owner <= 0 || base > UINTPTR_MAX - size) {
        *bad = DLSM_SHM_E_INVAL;
        return -1;
    }
    *out_owner = owner;
    *out_size  = size;
    if (out_base) { *out_base = base; }
    return 0;
}

dlsm_status dlsm_shm_create_or_recover(const char *name, size_t size, dlsm_shm **out) {
    if (!name || !out || size < sizeof(struct ShmHeader) + 16) {
        return DLSM_SHM_E_INVAL;
    }
    *out = NULL;
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            int rfd = shm_open(name, O_RDWR, 0600);
            if (rfd < 0) { return DLSM_SHM_E_OPEN; }
            int locked = writer_lock(rfd);
            if (locked == 0) { close(rfd); return DLSM_SHM_E_IN_USE; }
            if (locked < 0) { close(rfd); return DLSM_SHM_E_OPEN; }
            int32_t owner; uint64_t hsize; dlsm_status bad = DLSM_OK;
            if (read_header(rfd, &owner, &hsize, NULL, &bad) != 0) { close(rfd); return bad; }
            (void)owner;
            if (hsize != size) { close(rfd); return DLSM_SHM_E_INVAL; }
            /* stale: remap at the configured base and re-initialize the arena */
            void *rp = map_rw_at_base(rfd, (size_t)hsize);
            if (rp == MAP_FAILED) { close(rfd); return DLSM_SHM_E_BASE_OCCUPIED; }
            struct ShmHeader *rh = (struct ShmHeader *)rp;
            atomic_store_explicit(&rh->magic, 0, memory_order_release);
            memset((uint8_t *)rp + sizeof(*rh), 0, (size_t)hsize - sizeof(*rh));
            init_header((struct ShmHeader *)rp, (size_t)hsize);
            dlsm_shm *s = calloc(1, sizeof(*s));
            if (!s) { munmap(rp, (size_t)hsize); close(rfd); return DLSM_SHM_E_NOMEM; }
            s->hdr = (struct ShmHeader *)rp;
            s->mapped_size = (size_t)hsize;
            s->fd = rfd;
            s->readonly = false;
            *out = s;
            return DLSM_OK;
        }
        return DLSM_SHM_E_OPEN;
    }
    int locked = writer_lock(fd);
    if (locked != 1) {
        close(fd);
        shm_unlink(name);
        return DLSM_SHM_E_OPEN;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        shm_unlink(name);
        return DLSM_SHM_E_FTRUNCATE;
    }
    void *p = map_rw_at_base(fd, size);
    if (p == MAP_FAILED) {
        close(fd);
        shm_unlink(name);
        return DLSM_SHM_E_BASE_OCCUPIED;
    }
    init_header((struct ShmHeader *)p, size);

    dlsm_shm *s = calloc(1, sizeof(*s));
    if (!s) { munmap(p, size); close(fd); shm_unlink(name); return DLSM_SHM_E_NOMEM; }
    s->hdr = (struct ShmHeader *)p;
    s->mapped_size = size;
    s->fd = fd;
    s->readonly = false;
    *out = s;
    return DLSM_OK;
}

dlsm_status dlsm_shm_attach_readonly(const char *name, dlsm_shm **out) {
    if (!name || !out) { return DLSM_SHM_E_INVAL; }
    *out = NULL;
    int fd = shm_open(name, O_RDONLY, 0600);
    if (fd < 0) { return DLSM_SHM_E_OPEN; }
    int32_t owner; uint64_t hsize, hbase; dlsm_status bad = DLSM_OK;
    if (read_header(fd, &owner, &hsize, &hbase, &bad) != 0) { close(fd); return bad; }
    /* Map at the recorded base without replacing any existing mapping. */
    void *p = mmap((void *)(uintptr_t)hbase, (size_t)hsize, PROT_READ,
                   MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    if (p == MAP_FAILED || p != (void *)(uintptr_t)hbase) {
        if (p != MAP_FAILED) { munmap(p, (size_t)hsize); }
        close(fd);
        return DLSM_SHM_E_BASE_OCCUPIED;
    }
    dlsm_shm *s = calloc(1, sizeof(*s));
    if (!s) { munmap(p, (size_t)hsize); close(fd); return DLSM_SHM_E_NOMEM; }
    s->hdr = (struct ShmHeader *)p;
    s->mapped_size = (size_t)hsize;
    s->fd = fd;
    s->readonly = true;
    *out = s;
    return DLSM_OK;
}

void dlsm_shm_detach(dlsm_shm *s) {
    if (!s) { return; }
    munmap(s->hdr, s->mapped_size);
    close(s->fd);
    free(s);
}

void  *dlsm_shm_base(const dlsm_shm *s)     { return s->hdr; }
size_t dlsm_shm_capacity(const dlsm_shm *s) {
    return (size_t)atomic_load_explicit(&s->hdr->total_size, memory_order_acquire);
}
size_t dlsm_shm_used(const dlsm_shm *s) {
    return (size_t)atomic_load_explicit(&s->hdr->bump, memory_order_acquire);
}

static int is_pow2(size_t a) { return a != 0 && (a & (a - 1)) == 0; }

void *dlsm_shm_alloc(dlsm_shm *s, size_t size, size_t align) {
    if (!s || s->readonly || size == 0 || !is_pow2(align)) {
        return NULL;
    }
    struct ShmHeader *h = s->hdr;
    uint64_t cur = atomic_load_explicit(&h->bump, memory_order_acquire);
    uint64_t total = atomic_load_explicit(&h->total_size, memory_order_acquire);
    uintptr_t base = (uintptr_t)h;
    for (;;) {
        if (cur > total || cur > UINTPTR_MAX - base) { return NULL; }
        uintptr_t address = base + (uintptr_t)cur;
        if (address > UINTPTR_MAX - (align - 1)) { return NULL; }
        uintptr_t aligned_address = (address + (align - 1)) & ~((uintptr_t)align - 1);
        if (aligned_address < base) { return NULL; }
        uint64_t aligned = (uint64_t)(aligned_address - base);
        if (aligned > total || size > total - aligned) {
            return NULL; /* E_OOM */
        }
        uint64_t next = aligned + size;
        if (atomic_compare_exchange_weak_explicit(
                &h->bump, &cur, next,
                memory_order_acq_rel, memory_order_acquire)) {
            return (uint8_t *)h + aligned;
        }
        /* cur reloaded by CAS on failure; retry */
    }
}

bool dlsm_shm_offset_of(const dlsm_shm *s, const void *ptr, dlsm_shm_offset *out) {
    if (!s || !ptr || !out) { return false; }
    uintptr_t base = (uintptr_t)s->hdr;
    uintptr_t address = (uintptr_t)ptr;
    size_t capacity = dlsm_shm_capacity(s);
    if (address < base || address - base >= capacity) { return false; }
    *out = (dlsm_shm_offset)(address - base);
    return true;
}

void *dlsm_shm_pointer(const dlsm_shm *s, dlsm_shm_offset offset, size_t size) {
    if (!s) { return NULL; }
    size_t capacity = dlsm_shm_capacity(s);
    if (offset > capacity || size > capacity - (size_t)offset) { return NULL; }
    return (uint8_t *)s->hdr + offset;
}

dlsm_status dlsm_shm_cleanup_if_stale(const char *name, bool *cleaned) {
    if (cleaned) { *cleaned = false; }
    if (!name) { return DLSM_SHM_E_INVAL; }
    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
        return (errno == ENOENT) ? DLSM_OK : DLSM_SHM_E_OPEN;
    }
    int locked = writer_lock(fd);
    if (locked == 0) { close(fd); return DLSM_OK; }
    if (locked < 0) { close(fd); return DLSM_SHM_E_OPEN; }
    if (shm_unlink(name) != 0) { close(fd); return DLSM_SHM_E_OPEN; }
    close(fd);
    if (cleaned) { *cleaned = true; }
    return DLSM_OK;
}
