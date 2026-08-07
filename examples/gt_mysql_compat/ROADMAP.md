# GT MySQL Compatibility Roadmap

## Goal

Use a small number of dlsm virtual processors (VPs) to run many
`tpcc-mysql` connection workers without allowing a blocking GreatDB client
transport wait to block the carrying VP pthread.

The supported transports are:

- TCP
- Unix domain socket
- GreatDB Linux shared memory in `POLLING` mode
- GreatDB Linux shared memory in `FUTEX` mode

The SQL and transaction APIs remain synchronous. Compatibility code below
the MySQL client API converts transport waits into GT park/resume behavior.

## Constraints

- Do not change the GreatDB Linux SHM layout, ABI version, or peer protocol.
- GreatDB server behavior remains unchanged unless that process explicitly
  installs an adapter.
- A process that has not installed an adapter retains the existing polling
  and futex behavior.
- A native pthread in an adapter-enabled client retains the existing wait
  behavior; only a running GT uses cooperative waits.
- One `MYSQL *` and its statements belong to exactly one connection GT.
- Do not compile or run tests as part of these implementation steps.
- Do not modify MTR `.result` files.

## Phase 1: GreatDB Linux SHM wait injection

Status: completed in the GreatDB worktree; GreatDB build is supplied by the
user. The original review patch remains in `greatdb-shm-wait-adapter.patch`.

Add a process-local, optional Linux SHM wait adapter to GreatDB.

The injection point is `wait_for_condition()`, above the choice between
continuous polling and `SYS_futex`. This is required because an adapter at
`futex_wait()` alone would never be reached by `POLLING` mode.

Required behavior:

1. The adapter is null by default.
2. Registration affects only the registering process.
3. The adapter decides whether it applies to the current execution context.
4. If applicable, both `POLLING` and `FUTEX` connections perform their finite
   spin budget and then call the adapter.
5. If not applicable, the original polling/futex branches are unchanged.
6. The existing epoch/waiter publication and condition recheck order is
   retained to prevent lost wakeups.
7. Adapter wakeups may be spurious; `wait_for_condition()` always rechecks
   ring state, connection state, and deadline.
8. Existing `FUTEX_WAKE` calls remain unchanged for native waiters.

GreatDB compilation and binary production are performed by the user after
this phase.

## Phase 2: GT compatibility example

Status: Linux SHM and non-TLS TCP examples implemented; Unix wrapper remains
pending.

Create one example with selectable transport backends. Link the GreatDB
static client archive so GNU ld `--wrap` can intercept socket operations.

TCP and Unix socket paths:

- wrap `connect`, `recv`, `send`, and any confirmed VIO readiness syscall;
- call the real function outside GT context;
- use GT-aware nonblocking I/O and park/resume inside GT context;
- preserve partial I/O, timeout, `EINTR`, `EAGAIN`, and `EWOULDBLOCK` semantics.

The initial TCP implementation wraps `connect`, `recv`, `send`, `poll`, and
`ppoll`. It relies on GreatDB's finite-timeout nonblocking VIO state machine;
`poll`/`ppoll` readiness waits become cooperative GT timer sleeps. TLS is
explicitly disabled because calls made inside dynamically linked OpenSSL are
outside GNU ld `--wrap` interception.

The hook implementation is provided by the reusable static library under
`libs/mysql_compat`. The `dlsm::mysql_tcp_compat` INTERFACE target propagates
the required GNU ld options; the example contains no hook implementation.

The Linux SHM adapter is also implemented under `libs/mysql_compat`, exposed
separately as `dlsm::mysql_shm_compat` because it depends on GreatDB's
adapter contract. A minimal internal ABI declaration avoids requiring
`violite.h` in the installed client SDK; TCP-only consumers do not inherit the
SHM target.

Linux SHM path:

- register the Phase 1 process-local adapter;
- apply it to both GreatDB `POLLING` and `FUTEX` configurations;
- initially use cooperative timed epoch checks;
- keep the adapter boundary suitable for a later `futex_waitv` or io_uring
  futex backend.

The initial `gt_mysql_shm` executable covers both `shm-polling` and
`shm-futex`. It is opt-in through `DLSM_BUILD_GT_MYSQL_COMPAT` and is not
registered as an automatic test because it requires a configured GreatDB
server.

## Phase 3: Acceptance scenarios

Status: SHM and non-TLS TCP acceptance scenarios implemented; execution is
pending user build and server configuration. Unix remains blocked by its
wrapper.

For each backend (`tcp`, `unix`, `shm-polling`, and `shm-futex`):

1. Start a runtime with exactly one VP.
2. Spawn at least two GTs, each owning an independent connection.
3. Connection A executes a deliberately delayed query.
4. Connection B executes a fast query after A has started waiting.
5. B must finish before A.
6. Report transport, VP count, GT count, and ordered start/completion events.

This ordering demonstrates that a transport wait parks only connection A and
does not block the sole VP.

## Phase 4: tpcc-mysql integration

Status: blocked by Phase 3 acceptance

Make the smallest worker-lifecycle change:

- replace worker `pthread_create`/`pthread_join` with GT spawn/wait/release;
- keep the main pthread for signals, measurement timing, and reporting;
- keep connection initialization, prepared statements, transaction loop, and
  connection teardown in the owning GT;
- configure VP count independently from connection count;
- enable the socket wrappers or Linux SHM adapter according to transport;
- retain pthread synchronization that is shared between the main pthread and
  GTs until a mixed-context replacement is designed.

## Phase 5: Scale and correctness hardening

Status: blocked by Phase 4

Define tests before further optimization for:

- many connections on one VP;
- concurrent read/write readiness;
- timeout versus readiness races;
- GT cancellation and transaction rollback;
- connection close and descriptor reuse;
- SHM epoch changes during adapter registration;
- runtime drain while connections are waiting;
- client pthread fallback after adapter registration;
- global sequence and percentile-lock contention in `tpcc-mysql`.

Only after correctness is established should the cooperative SHM polling
backend be replaced with a batched kernel wait facility.
