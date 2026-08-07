# GreatDB Linux SHM compatibility example

The examples demonstrate that synchronous MySQL C API calls can run on two
green threads carried by one VP. `gt_mysql_shm` uses the injected Linux SHM
adapter. `gt_mysql_tcp` uses GNU ld `--wrap` around the GreatDB static client
socket calls through the reusable `dlsm::mysql_tcp_compat` target. Both
convert transport waits to GT timer sleeps.

The executables are deliberately not automatic tests. They require a running
GreatDB server and two permitted client connections; the SHM executable also
requires the Linux SHM listener.

## Build configuration

GreatDB must be rebuilt and installed after the adapter change. Configure dlsm
with the example enabled and point it at the installed client SDK:

```sh
cmake -S /home/moyu/learn/dlsmc -B <dlsm-build-dir> \
  -DDLSM_BUILD_GT_MYSQL_COMPAT=ON \
  -DDLSM_MYSQL_ROOT=<greatdb-install-prefix>
cmake --build <dlsm-build-dir> --target gt_mysql_shm gt_mysql_tcp
```

The prefix must provide `mysql.h` and a static `libperconaserverclient.a` or
`libmysqlclient.a`. It does not need to provide `violite.h` or `mysql_config`.
Nonstandard layouts can override `DLSM_MYSQL_INCLUDE_DIR`,
`DLSM_MYSQL_CLIENT_ARCHIVE`, `DLSM_MYSQL_LIBRARY_DIR`, and
`DLSM_MYSQL_PRIVATE_LIBRARY_DIR`. Override the semicolon-separated
`DLSM_MYSQL_LINK_LIBRARIES` when the static client has a different dependency
set.

## Acceptance runs

Run both server-compatible wait modes. `MYSQL` below is the SHM base name,
not a TCP host:

```sh
<dlsm-build-dir>/examples/gt_mysql_compat/gt_mysql_shm \
  shm-polling MYSQL <user> <password> [database] [slow-seconds] [spin-count]

<dlsm-build-dir>/examples/gt_mysql_compat/gt_mysql_shm \
  shm-futex MYSQL <user> <password> [database] [slow-seconds] [spin-count]
```

The default delayed query is `SELECT SLEEP(2)`. A successful run reports one
VP, two GTs, a nonzero adapter wait count, and `fast_order=1 slow_order=2`.
It exits nonzero if either connection/query fails or the slow query finishes
first.

Supplying a password on the command line can expose it through process-listing
tools; use a disposable test account for this acceptance example.

For TCP, the example forces `SSL_MODE_DISABLED` and finite connect/read/write
timeouts. This keeps every potentially blocking GreatDB VIO call on its
nonblocking `poll`/`ppoll` path:

```sh
<dlsm-build-dir>/examples/gt_mysql_compat/gt_mysql_tcp \
  <host> <port> <user> <password> [database] [slow-seconds]
```

A successful TCP run additionally reports nonzero wrapper wait and cooperative
sleep counts. TLS is intentionally unsupported in this phase: socket calls
originating inside dynamically linked OpenSSL cannot be intercepted by the
executable's GNU ld wrappers.

## Reusing the TCP wrapper

The implementation lives under `libs/mysql_compat`, not in the example. Link
the compatibility target after adding dlsm to a CMake build:

```cmake
target_link_libraries(tpcc_mysql PRIVATE dlsm::mysql_tcp_compat)
```

The INTERFACE target links `libdlsm_mysql_tcp_wrap.a` and propagates all five
GNU ld `--wrap` options. C and C++ callers include
`dlsm/mysql_tcp_wrap.h`, enable the process-level wrapper before starting
connection GTs, and disable it only after all those GTs have completed.

The SHM adapter implementation is in the same `libs/mysql_compat` directory
but remains a separate `dlsm::mysql_shm_compat` target. Build it with
`DLSM_BUILD_MYSQL_SHM_COMPAT=ON`. Consumers include
`dlsm/mysql_shm_adapter.h`; adapter enable/disable must happen while GreatDB
client connection activity is quiescent. Its internal minimal ABI declaration
must be synchronized if GreatDB changes the adapter contract.
