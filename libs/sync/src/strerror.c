#include "dlsm/sync.h"

const char *dlsm_sync_strerror(dlsm_status st) {
    switch (st) {
#define DLSM_SYNC_MSG_X(name, code, msg) case name: return msg;
    DLSM_SYNC_ERROR_LIST(DLSM_SYNC_MSG_X)
#undef DLSM_SYNC_MSG_X
    default: return DLSM_MSG_UNKNOWN;
    }
}
