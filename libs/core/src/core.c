#include "dlsm/core.h"

const char *dlsm_strerror(dlsm_status st) {
    switch (st) {
    case DLSM_OK: return DLSM_MSG_OK;
    default:      return DLSM_MSG_UNKNOWN;
    }
}
