#include "dlsm/greenthread.h"
#include "dlsm/mysql_tcp_wrap.h"
#include "mysql.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

namespace {

struct Config {
  const char *host;
  unsigned int port;
  const char *user;
  const char *password;
  const char *database;
  unsigned int slow_seconds;
};

struct Run_state {
  Config config;
  std::atomic<bool> slow_started{false};
  std::atomic<unsigned int> completion_order{0};
  std::atomic<unsigned int> slow_order{0};
  std::atomic<unsigned int> fast_order{0};
  std::atomic<unsigned int> failures{0};
};

MYSQL *connect_one(const Config &config) {
  MYSQL *mysql = mysql_init(nullptr);
  if (mysql == nullptr) return nullptr;

  unsigned int timeout_seconds = 10;
  mysql_ssl_mode ssl_mode = SSL_MODE_DISABLED;
  mysql_protocol_type protocol = MYSQL_PROTOCOL_TCP;
  if (mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds) != 0 ||
      mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, &timeout_seconds) != 0 ||
      mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT, &timeout_seconds) != 0 ||
      mysql_options(mysql, MYSQL_OPT_SSL_MODE, &ssl_mode) != 0 ||
      mysql_options(mysql, MYSQL_OPT_PROTOCOL, &protocol) != 0 ||
      mysql_real_connect(mysql, config.host, config.user, config.password,
                         config.database, config.port, nullptr, 0) == nullptr) {
    std::fprintf(stderr, "connect failed: %s\n", mysql_error(mysql));
    mysql_close(mysql);
    return nullptr;
  }
  return mysql;
}

bool execute_and_drain(MYSQL *mysql, const char *sql) {
  if (mysql_query(mysql, sql) != 0) {
    std::fprintf(stderr, "query failed (%s): %s\n", sql, mysql_error(mysql));
    return false;
  }
  MYSQL_RES *result = mysql_store_result(mysql);
  if (result != nullptr) mysql_free_result(result);
  if (result == nullptr && mysql_field_count(mysql) != 0) {
    std::fprintf(stderr, "result failed (%s): %s\n", sql, mysql_error(mysql));
    return false;
  }
  return true;
}

void slow_connection(void *opaque) {
  auto *state = static_cast<Run_state *>(opaque);
  MYSQL *mysql = connect_one(state->config);
  if (mysql == nullptr) {
    state->failures.fetch_add(1, std::memory_order_relaxed);
    state->slow_started.store(true, std::memory_order_release);
    return;
  }
  char sql[64];
  std::snprintf(sql, sizeof(sql), "SELECT SLEEP(%u)",
                state->config.slow_seconds);
  state->slow_started.store(true, std::memory_order_release);
  if (!execute_and_drain(mysql, sql))
    state->failures.fetch_add(1, std::memory_order_relaxed);
  state->slow_order.store(
      state->completion_order.fetch_add(1, std::memory_order_relaxed) + 1,
      std::memory_order_release);
  mysql_close(mysql);
}

void fast_connection(void *opaque) {
  auto *state = static_cast<Run_state *>(opaque);
  while (!state->slow_started.load(std::memory_order_acquire)) dlsm_gt_yield();
  MYSQL *mysql = connect_one(state->config);
  if (mysql == nullptr) {
    state->failures.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!execute_and_drain(mysql, "SELECT 1"))
    state->failures.fetch_add(1, std::memory_order_relaxed);
  state->fast_order.store(
      state->completion_order.fetch_add(1, std::memory_order_relaxed) + 1,
      std::memory_order_release);
  mysql_close(mysql);
}

void usage(const char *program) {
  std::fprintf(stderr,
      "usage: %s <host> <port> <user> <password> [database] [slow-seconds]\n",
      program);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 5) {
    usage(argv[0]);
    return 2;
  }
  Run_state state;
  state.config = Config{
      argv[1], static_cast<unsigned int>(std::strtoul(argv[2], nullptr, 10)),
      argv[3], argv[4], argc > 5 ? argv[5] : nullptr,
      argc > 6 ? static_cast<unsigned int>(std::strtoul(argv[6], nullptr, 10))
               : 2U};

  if (mysql_library_init(0, nullptr, nullptr) != 0) {
    std::fprintf(stderr, "mysql_library_init failed\n");
    return 2;
  }
  dlsm_mysql_tcp_wrap_enable();

  dlsm_gt_runtime *runtime = dlsm_gt_runtime_new(1, 0);
  dlsm_gt_task *slow = runtime ? dlsm_gt_spawn(runtime, slow_connection, &state)
                               : nullptr;
  dlsm_gt_task *fast = runtime ? dlsm_gt_spawn(runtime, fast_connection, &state)
                               : nullptr;
  const dlsm_status run_status = runtime && slow && fast
                                     ? dlsm_gt_run(runtime)
                                     : DLSM_GT_E_STATE;

  dlsm_gt_stats gt_stats{};
  if (runtime != nullptr) (void)dlsm_gt_runtime_stats(runtime, &gt_stats);
  if (slow != nullptr) (void)dlsm_gt_task_release(slow);
  if (fast != nullptr) (void)dlsm_gt_task_release(fast);
  if (runtime != nullptr) (void)dlsm_gt_runtime_free(runtime);

  dlsm_mysql_tcp_wrap_disable();
  const dlsm_mysql_tcp_wrap_stats wrap_stats =
      dlsm_mysql_tcp_wrap_get_stats();
  mysql_library_end();

  const unsigned int slow_order = state.slow_order.load(std::memory_order_acquire);
  const unsigned int fast_order = state.fast_order.load(std::memory_order_acquire);
  const bool passed = run_status == DLSM_OK &&
                      state.failures.load(std::memory_order_relaxed) == 0 &&
                      wrap_stats.connect_calls != 0 &&
                      wrap_stats.recv_calls != 0 && wrap_stats.send_calls != 0 &&
                      wrap_stats.wait_calls != 0 &&
                      wrap_stats.cooperative_sleeps != 0 && fast_order != 0 &&
                      slow_order != 0 && fast_order < slow_order;

  std::printf("transport=tcp ssl=disabled vp=1 gt=2 connect=%" PRIu64
              " recv=%" PRIu64 " send=%" PRIu64 " waits=%" PRIu64
              " cooperative_sleeps=%" PRIu64 " parks=%" PRIu64
              " fast_order=%u slow_order=%u\n",
              wrap_stats.connect_calls, wrap_stats.recv_calls,
              wrap_stats.send_calls, wrap_stats.wait_calls,
              wrap_stats.cooperative_sleeps, gt_stats.parks, fast_order,
              slow_order);
  std::printf("RESULT: %s\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}
