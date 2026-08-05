static _Thread_local int task_state;

int read_task_state(void) {
    return task_state;
}

