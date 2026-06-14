/* Example: a concurrent TCP echo server on the dlsm runtime.
 *
 * Demonstrates that the base libraries are general infrastructure, not just
 * bwtree glue: an echo server falls out of greenthread + dlsm-io with no new
 * machinery. One acceptor green thread loops on dlsm_io_accept and spawns a
 * handler green thread per connection; each handler loops recv -> send. All of
 * it runs across multiple worker cores, every syscall going through io_uring
 * (submit -> park -> reaper unpark), so thousands of connections can be in
 * flight on a handful of OS threads.
 *
 * Self-checking: N client OS threads connect to 127.0.0.1, send a unique
 * message, and verify they get the same bytes back.
 *
 * Usage: echo_server [nworkers=4] [nclients=32]
 */
#define _GNU_SOURCE
#include "dlsm/io.h"
#include "dlsm/greenthread.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static _Atomic int g_conns_handled;

/* ---- server side (green threads) ---- */
typedef struct { dlsm_io *io; int fd; } conn_ctx;

static void handler(void *arg) {
    conn_ctx *c = (conn_ctx *)arg;
    char buf[512];
    for (;;) {
        ssize_t n = dlsm_io_recv(c->io, c->fd, buf, sizeof buf);
        if (n <= 0) { break; }                 /* 0 = peer closed */
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = dlsm_io_send(c->io, c->fd, buf + off, (size_t)(n - off));
            if (w <= 0) { break; }
            off += w;
        }
    }
    close(c->fd);
    free(c);
    atomic_fetch_add_explicit(&g_conns_handled, 1, memory_order_relaxed);
}

typedef struct { dlsm_io *io; dlsm_gt_runtime *rt; int listen_fd; int n; } acc_ctx;

static void acceptor(void *arg) {
    acc_ctx *a = (acc_ctx *)arg;
    for (int i = 0; i < a->n; i++) {
        int cfd = dlsm_io_accept(a->io, a->listen_fd, NULL, NULL); /* parks until a conn arrives */
        if (cfd < 0) { break; }
        conn_ctx *c = malloc(sizeof *c);
        c->io = a->io;
        c->fd = cfd;
        dlsm_gt_spawn(a->rt, handler, c);
    }
}

/* ---- client side (plain OS threads) ---- */
typedef struct { int port; int id; int ok; } client_ctx;

static int send_all(int fd, const char *p, size_t n) {
    size_t off = 0;
    while (off < n) { ssize_t w = send(fd, p + off, n - off, 0); if (w <= 0) { return -1; } off += (size_t)w; }
    return 0;
}
static int recv_all(int fd, char *p, size_t n) {
    size_t off = 0;
    while (off < n) { ssize_t r = recv(fd, p + off, n - off, 0); if (r <= 0) { return -1; } off += (size_t)r; }
    return 0;
}

static void *client_main(void *arg) {
    client_ctx *c = (client_ctx *)arg;
    c->ok = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return NULL; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)c->port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return NULL; }

    char msg[64];
    int len = snprintf(msg, sizeof msg, "ping-from-client-%d", c->id);
    char back[64];
    memset(back, 0, sizeof back);
    if (send_all(fd, msg, (size_t)len) == 0 &&
        recv_all(fd, back, (size_t)len) == 0 &&
        memcmp(msg, back, (size_t)len) == 0) {
        c->ok = 1;
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    int nworkers = (argc > 1) ? atoi(argv[1]) : 4;
    int nclients = (argc > 2) ? atoi(argv[2]) : 32;
    if (nworkers <= 0) { nworkers = 4; }
    if (nclients <= 0) { nclients = 32; }

    /* listening socket on an ephemeral loopback port */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) { perror("bind"); return 2; }
    if (listen(lfd, 128) != 0) { perror("listen"); return 2; }
    socklen_t slen = sizeof sa;
    getsockname(lfd, (struct sockaddr *)&sa, &slen);
    int port = ntohs(sa.sin_port);

    printf("== dlsm echo server (io_uring + green threads) ==\n");
    printf("workers=%d  clients=%d  port=%d\n\n", nworkers, nclients, port);
    fflush(stdout);

    /* start clients first; their connects queue in the listen backlog */
    pthread_t *th = calloc((size_t)nclients, sizeof *th);
    client_ctx *cc = calloc((size_t)nclients, sizeof *cc);
    for (int i = 0; i < nclients; i++) {
        cc[i].port = port;
        cc[i].id = i;
        pthread_create(&th[i], NULL, client_main, &cc[i]);
    }

    /* run the server: accept nclients connections, echo, finish */
    dlsm_io *io = dlsm_io_new(256);
    if (!io) { fprintf(stderr, "dlsm_io_new failed\n"); return 2; }
    dlsm_gt_runtime *rt = dlsm_gt_runtime_new(nworkers, 0);
    acc_ctx a = { io, rt, lfd, nclients };
    dlsm_gt_spawn(rt, acceptor, &a);
    dlsm_gt_run(rt);
    dlsm_gt_runtime_free(rt);
    dlsm_io_free(io);

    int ok = 0;
    for (int i = 0; i < nclients; i++) { pthread_join(th[i], NULL); ok += cc[i].ok; }
    close(lfd);
    free(th);
    free(cc);

    int handled = atomic_load(&g_conns_handled);
    printf("-- summary --\n");
    printf("clients ok=%d/%d  connections handled=%d  workers=%d\n",
           ok, nclients, handled, nworkers);
    if (ok != nclients) { printf("RESULT: FAIL\n"); return 1; }
    printf("RESULT: PASS (all clients echoed correctly)\n");
    return 0;
}
