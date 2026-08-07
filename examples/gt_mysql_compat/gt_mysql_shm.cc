#include "dlsm/greenthread.h"
#include "dlsm/mysql_shm_adapter.h"
#include "mysql.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Config {
  const char *transport;
  const char *base_name;
  const char *user;
  const char *password;
  const char *database;
  unsigned int wait_mode;
  unsigned int spin_count;
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

  MYSQL_LINUX_SHM_CONFIG shm_config{config.base_name, config.wait_mode,
                                    config.spin_count};
  if (mysql_options(mysql, MYSQL_OPT_LINUX_SHM, &shm_config) != 0 ||
      mysql_real_connect(mysql, nullptr, config.user, config.password,
                         config.database, 0, nullptr, 0) == nullptr) {
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
      "usage: %s <shm-polling|shm-futex> <base-name> <user> <password> "
      "[database] [slow-seconds] [spin-count]\n", program);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 5 || (std::strcmp(argv[1], "shm-polling") != 0 &&
                   std::strcmp(argv[1], "shm-futex") != 0)) {
    usage(argv[0]);
    return 2;
  }

  Run_state state;
  state.config = Config{
      argv[1], argv[2], argv[3], argv[4], argc > 5 ? argv[5] : nullptr,
      std::strcmp(argv[1], "shm-polling") == 0
          ? MYSQL_LINUX_SHM_WAIT_POLLING
          : MYSQL_LINUX_SHM_WAIT_FUTEX,
      argc > 7 ? static_cast<unsigned int>(std::strtoul(argv[7], nullptr, 10))
               : 1000U,
      argc > 6 ? static_cast<unsigned int>(std::strtoul(argv[6], nullptr, 10))
               : 2U};

  if (mysql_library_init(0, nullptr, nullptr) != 0) {
    std::fprintf(stderr, "mysql_library_init failed\n");
    return 2;
  }

  dlsm_mysql_shm_adapter_enable();

  dlsm_gt_runtime *runtime = dlsm_gt_runtime_new(1, 0);
  dlsm_gt_task *slow = runtime ? dlsm_gt_spawn(runtime, slow_connection, &state)
                               : nullptr;
  dlsm_gt_task *fast = runtime ? dlsm_gt_spawn(runtime, fast_connection, &state)
                               : nullptr;
  dlsm_status run_status = runtime && slow && fast ? dlsm_gt_run(runtime)
                                                   : DLSM_GT_E_STATE;

  dlsm_gt_stats stats{};
  if (runtime != nullptr) (void)dlsm_gt_runtime_stats(runtime, &stats);
  if (slow != nullptr) (void)dlsm_gt_task_release(slow);
  if (fast != nullptr) (void)dlsm_gt_task_release(fast);
  if (runtime != nullptr) (void)dlsm_gt_runtime_free(runtime);

  dlsm_mysql_shm_adapter_disable();
  const dlsm_mysql_shm_adapter_stats adapter_stats =
      dlsm_mysql_shm_adapter_get_stats();
  mysql_library_end();

  const unsigned int slow_order = state.slow_order.load(std::memory_order_acquire);
  const unsigned int fast_order = state.fast_order.load(std::memory_order_acquire);
  const bool passed = run_status == DLSM_OK &&
                      state.failures.load(std::memory_order_relaxed) == 0 &&
                      adapter_stats.wait_calls != 0 &&
                      adapter_stats.cooperative_sleeps != 0 &&
                      fast_order != 0 && slow_order != 0 && fast_order < slow_order;

  std::printf("transport=%s vp=1 gt=2 adapter_waits=%" PRIu64
              " adapter_sleeps=%" PRIu64 " parks=%" PRIu64
              " fast_order=%u slow_order=%u\n",
              state.config.transport,
              adapter_stats.wait_calls, adapter_stats.cooperative_sleeps,
              stats.parks,
              fast_order, slow_order);
  std::printf("RESULT: %s\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}
