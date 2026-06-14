#include "dlsm/index.h"

const char *dlsm_index_strerror(dlsm_status st) {
    switch (st) {
#define DLSM_INDEX_MSG_X(name, code, msg) case name: return msg;
    DLSM_INDEX_ERROR_LIST(DLSM_INDEX_MSG_X)
#undef DLSM_INDEX_MSG_X
    default: return DLSM_MSG_UNKNOWN;
    }
}
