/* Guard-page protection: a green thread that overflows its stack must hit the
 * PROT_NONE guard page and die with SIGSEGV — not silently corrupt a neighbour.
 * We run the overflow in a forked child and assert the child is killed by
 * SIGSEGV. The parent (this test process) returns 0 on success. */
#include "dlsm/greenthread.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/* Recurse with a touched buffer so the compiler cannot elide frames; this
 * marches the stack pointer down into the guard page. */
__attribute__((noinline))
static void blow(volatile char *sink, int depth) {
    volatile char buf[1024];
    buf[0] = (char)depth;
    buf[1023] = (char)(depth ^ 0x5a);
    if (sink) { buf[1] = sink[0]; }
    blow(buf, depth + 1);
}

static void overflow_task(void *arg) {
    (void)arg;
    blow(NULL, 0);
}

int main(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) {
        /* small stack so we reach the guard quickly */
        dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 64 * 1024);
        dlsm_gt_spawn(rt, overflow_task, NULL);
        dlsm_gt_run(rt);          /* expected to fault and die */
        _exit(0);                 /* if we got here, the guard did NOT trigger */
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV) {
        printf("guard ok: stack overflow trapped by guard page (SIGSEGV)\n");
        return 0;
    }
    fprintf(stderr, "guard FAILED: child did not die with SIGSEGV (status=%d)\n", st);
    return 1;
}
