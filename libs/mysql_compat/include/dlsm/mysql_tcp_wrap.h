#ifndef DLSM_MYSQL_TCP_WRAP_H
#define DLSM_MYSQL_TCP_WRAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dlsm_mysql_tcp_wrap_stats {
    uint64_t connect_calls;
    uint64_t recv_calls;
    uint64_t send_calls;
    uint64_t wait_calls;
    uint64_t cooperative_sleeps;
} dlsm_mysql_tcp_wrap_stats;

void dlsm_mysql_tcp_wrap_enable(void);
void dlsm_mysql_tcp_wrap_disable(void);
dlsm_mysql_tcp_wrap_stats dlsm_mysql_tcp_wrap_get_stats(void);

#ifdef __cplusplus
}
#endif

#endif
