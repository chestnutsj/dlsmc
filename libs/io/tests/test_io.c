#include "unity.h"
#include "dlsm/io.h"
#include "dlsm/greenthread.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void setUp(void) {}
void tearDown(void) {}

typedef struct { dlsm_io *io; int result; } job;

/* anonymous temp file: open + unlink, keep the fd */
static int temp_fd(void) {
    char path[] = "/tmp/dlsm_io_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) { unlink(path); }
    return fd;
}

/* --- strerror (no runtime) --- */
static void test_strerror(void) {
    TEST_ASSERT_EQUAL_STRING("io_uring_setup failed", dlsm_io_strerror(DLSM_IO_E_SETUP));
    TEST_ASSERT_EQUAL_STRING("invalid argument", dlsm_io_strerror(DLSM_IO_E_INVAL));
    TEST_ASSERT_EQUAL_STRING("unknown error", dlsm_io_strerror(1));
}

/* --- calling outside a green thread is rejected --- */
static void test_outside_green_thread(void) {
    dlsm_io *io = dlsm_io_new(8);
    TEST_ASSERT_NOT_NULL(io);
    char b[4];
    errno = 0;
    ssize_t r = dlsm_io_read_at(io, 0, b, sizeof b, 0);
    TEST_ASSERT_EQUAL_INT(-1, r);
    TEST_ASSERT_EQUAL_INT(ENOTSUP, errno);
    dlsm_io_free(io);
}

/* --- write -> fsync -> read round trip --- */
static void roundtrip_task(void *arg) {
    job *j = (job *)arg;
    j->result = -1;
    int fd = temp_fd();
    if (fd < 0) { j->result = 10; return; }
    const char msg[] = "dlsm-io roundtrip payload 0123456789";
    if (dlsm_io_write_at(j->io, fd, msg, sizeof msg, 0) != (ssize_t)sizeof msg) { j->result = 1; close(fd); return; }
    if (dlsm_io_fsync(j->io, fd) != 0) { j->result = 2; close(fd); return; }
    char buf[64];
    memset(buf, 0, sizeof buf);
    ssize_t r = dlsm_io_read_at(j->io, fd, buf, sizeof msg, 0);
    if (r != (ssize_t)sizeof msg || memcmp(buf, msg, sizeof msg) != 0) { j->result = 3; close(fd); return; }
    close(fd);
    j->result = 0;
}

static void test_roundtrip(void) {
    dlsm_io *io = dlsm_io_new(64);
    TEST_ASSERT_NOT_NULL(io);
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    job j = { io, -1 };
    dlsm_gt_spawn(rt, roundtrip_task, &j);
    dlsm_gt_run(rt);
    dlsm_gt_runtime_free(rt);
    dlsm_io_free(io);
    TEST_ASSERT_EQUAL_INT(0, j.result);
}

/* --- short read at EOF returns the available byte count --- */
static void eof_task(void *arg) {
    job *j = (job *)arg;
    int fd = temp_fd();
    char data[100];
    memset(data, 'x', sizeof data);
    dlsm_io_write_at(j->io, fd, data, sizeof data, 0);
    dlsm_io_fsync(j->io, fd);
    char buf[200];
    ssize_t r = dlsm_io_read_at(j->io, fd, buf, sizeof buf, 0); /* asks 200, file has 100 */
    j->result = (r == 100) ? 0 : 1;
    close(fd);
}

static void test_short_read_at_eof(void) {
    dlsm_io *io = dlsm_io_new(16);
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
    job j = { io, -1 };
    dlsm_gt_spawn(rt, eof_task, &j);
    dlsm_gt_run(rt);
    dlsm_gt_runtime_free(rt);
    dlsm_io_free(io);
    TEST_ASSERT_EQUAL_INT(0, j.result);
}

/* --- many green threads doing I/O concurrently across workers --- */
#define NCONC 32
static void test_concurrent_io(void) {
    dlsm_io *io = dlsm_io_new(64);
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(4, 0);
    static job jobs[NCONC];
    for (int i = 0; i < NCONC; i++) {
        jobs[i] = (job){ io, -1 };
        dlsm_gt_spawn(rt, roundtrip_task, &jobs[i]);
    }
    dlsm_gt_run(rt);
    dlsm_gt_runtime_free(rt);
    dlsm_io_free(io);
    for (int i = 0; i < NCONC; i++) { TEST_ASSERT_EQUAL_INT(0, jobs[i].result); }
}

/* --- property: random offset/len write/read round trips (fixed seed) --- */
static void prop_task(void *arg) {
    job *j = (job *)arg;
    j->result = -1;
    int fd = temp_fd();
    uint64_t s = 0xBADC0FFEE0DDF00DULL;
    unsigned char wbuf[512], rbuf[512];
    for (int it = 0; it < 300; it++) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        uint64_t x = s * 0x2545F4914F6CDD1DULL;
        off_t off = (off_t)(x % 4096);
        size_t len = (size_t)(1 + (x >> 16) % 512);
        for (size_t k = 0; k < len; k++) { wbuf[k] = (unsigned char)(off + k + it); }
        if (dlsm_io_write_at(j->io, fd, wbuf, len, off) != (ssize_t)len) { j->result = 1; close(fd); return; }
        memset(rbuf, 0, sizeof rbuf);
        if (dlsm_io_read_at(j->io, fd, rbuf, len, off) != (ssize_t)len) { j->result = 2; close(fd); return; }
        if (memcmp(wbuf, rbuf, len) != 0) { j->result = 3; close(fd); return; }
    }
    close(fd);
    j->result = 0;
}

static void test_property_roundtrip(void) {
    dlsm_io *io = dlsm_io_new(32);
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    job j = { io, -1 };
    dlsm_gt_spawn(rt, prop_task, &j);
    dlsm_gt_run(rt);
    dlsm_gt_runtime_free(rt);
    dlsm_io_free(io);
    TEST_ASSERT_EQUAL_INT(0, j.result);
}

/* --- socket recv/send over a socketpair, on two green threads --- */
typedef struct { dlsm_io *io; int fd; int result; const char *msg; size_t len; } sock_job;

static void sender(void *arg) {
    sock_job *j = (sock_job *)arg;
    ssize_t n = dlsm_io_send(j->io, j->fd, j->msg, j->len);
    j->result = (n == (ssize_t)j->len) ? 0 : 1;
}
static void receiver(void *arg) {
    sock_job *j = (sock_job *)arg;
    char buf[64];
    memset(buf, 0, sizeof buf);
    ssize_t n = dlsm_io_recv(j->io, j->fd, buf, sizeof buf);
    j->result = (n == (ssize_t)j->len && memcmp(buf, j->msg, j->len) == 0) ? 0 : 1;
}

static void test_socket_recv_send(void) {
    int sv[2];
    TEST_ASSERT_EQUAL_INT(0, socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    dlsm_io *io = dlsm_io_new(16);
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
    const char *msg = "echo-via-io_uring";
    size_t len = strlen(msg);
    sock_job s = { io, sv[0], -1, msg, len };
    sock_job r = { io, sv[1], -1, msg, len };
    dlsm_gt_spawn(rt, receiver, &r);   /* parks on recv until the sender runs */
    dlsm_gt_spawn(rt, sender, &s);
    dlsm_gt_run(rt);
    dlsm_gt_runtime_free(rt);
    dlsm_io_free(io);
    close(sv[0]);
    close(sv[1]);
    TEST_ASSERT_EQUAL_INT(0, s.result);
    TEST_ASSERT_EQUAL_INT(0, r.result);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_strerror);
    RUN_TEST(test_outside_green_thread);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_short_read_at_eof);
    RUN_TEST(test_concurrent_io);
    RUN_TEST(test_property_roundtrip);
    RUN_TEST(test_socket_recv_send);
    return UNITY_END();
}
