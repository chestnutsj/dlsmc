#include <unistd.h>

void potentially_blocking(int fd, void *buffer) {
    (void)read(fd, buffer, 1);
}
