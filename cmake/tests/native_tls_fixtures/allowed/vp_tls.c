/* This state deliberately belongs to a physical pthread. */
static _Thread_local int vp_state; /* DLSM_GT_NATIVE_TLS_ALLOWED */

int read_vp_state(void) {
    return vp_state;
}

