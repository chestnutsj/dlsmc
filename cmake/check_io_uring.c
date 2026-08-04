#define _GNU_SOURCE
#include <linux/io_uring.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    int fd = (int)syscall(__NR_io_uring_setup, 2u, &params);
    if (fd < 0) { return 1; }
    close(fd);
    return 0;
}
