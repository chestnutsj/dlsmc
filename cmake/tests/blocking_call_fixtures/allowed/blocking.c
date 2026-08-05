#include <unistd.h>

void audited_physical_thread_read(int fd, void *buffer) {
    (void)read(fd, buffer, 1); /* DLSM_GT_BLOCKING_CALL_ALLOWED */
}
