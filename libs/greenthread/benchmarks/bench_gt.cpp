#include <benchmark/benchmark.h>

extern "C" {
#include "dlsm/greenthread.h"
#include "bench_gt_bridge.h"
}

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <vector>

namespace {

struct YieldArgs {
    std::int64_t count;
};

void yield_task(void *opaque) {
    const auto *args = static_cast<const YieldArgs *>(opaque);
    for (std::int64_t i = 0; i < args->count; ++i) {
        dlsm_gt_yield();
    }
}

void BM_ContextSwitch(benchmark::State &state) {
    const YieldArgs args{state.range(0)};
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
        dlsm_gt_spawn(rt, yield_task, const_cast<YieldArgs *>(&args));
        state.ResumeTiming();
        dlsm_status status = dlsm_gt_run(rt);
        benchmark::DoNotOptimize(status);
        state.PauseTiming();
        dlsm_gt_runtime_free(rt);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * args.count);
    state.counters["yields/s"] = benchmark::Counter(
        static_cast<double>(state.iterations() * args.count),
        benchmark::Counter::kIsRate);
}

void BM_PThreadSchedYield(benchmark::State &state) {
    const std::int64_t count = state.range(0);
    for (auto _ : state) {
        (void)_;
        for (std::int64_t i = 0; i < count; ++i) {
            int status = sched_yield();
            benchmark::DoNotOptimize(status);
        }
    }
    state.SetItemsProcessed(state.iterations() * count);
}

void empty_task(void *) {}

void *pthread_empty_task(void *) { return nullptr; }

void BM_SpawnAndFinish(benchmark::State &state) {
    const int tasks = static_cast<int>(state.range(0));
    for (auto _ : state) {
        (void)_;
        dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
        for (int i = 0; i < tasks; ++i) {
            dlsm_gt_task *task = dlsm_gt_spawn(rt, empty_task, nullptr);
            benchmark::DoNotOptimize(task);
        }
        dlsm_status status = dlsm_gt_run(rt);
        benchmark::DoNotOptimize(status);
        dlsm_gt_runtime_free(rt);
    }
    state.SetItemsProcessed(state.iterations() * tasks);
}

void BM_SpawnDetachedAndReclaim(benchmark::State &state) {
    const int tasks = static_cast<int>(state.range(0));
    for (auto _ : state) {
        (void)_;
        dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
        bool submit_failed = false;
        for (int i = 0; i < tasks; ++i) {
            dlsm_status status =
                dlsm_gt_spawn_detached(rt, empty_task, nullptr, nullptr);
            benchmark::DoNotOptimize(status);
            if (status != DLSM_OK) {
                submit_failed = true;
                break;
            }
        }
        dlsm_status status = dlsm_gt_run(rt);
        dlsm_gt_stats stats{};
        dlsm_gt_runtime_stats(rt, &stats);
        benchmark::DoNotOptimize(status);
        benchmark::DoNotOptimize(stats.task_controls);
        if (submit_failed || status != DLSM_OK || stats.task_controls != 0) {
            state.SkipWithError("detached GT was not reclaimed");
        }
        dlsm_gt_runtime_free(rt);
        if (submit_failed || status != DLSM_OK || stats.task_controls != 0) {
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * tasks);
}

void BM_PThreadCreateJoin(benchmark::State &state) {
    const int tasks = static_cast<int>(state.range(0));
    std::vector<pthread_t> threads(static_cast<std::size_t>(tasks));
    for (auto _ : state) {
        (void)_;
        int created = 0;
        for (; created < tasks; ++created) {
            if (pthread_create(&threads[static_cast<std::size_t>(created)], nullptr,
                               pthread_empty_task, nullptr) != 0) {
                break;
            }
        }
        for (int i = 0; i < created; ++i) {
            pthread_join(threads[static_cast<std::size_t>(i)], nullptr);
        }
        if (created != tasks) {
            state.SkipWithError("pthread_create failed");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * tasks);
}

std::atomic<dlsm_gt_task *> parked_task;

void parker_task(void *opaque) {
    const auto count = *static_cast<const std::int64_t *>(opaque);
    parked_task.store(dlsm_gt_self(), std::memory_order_release);
    for (std::int64_t i = 0; i < count; ++i) {
        dlsm_gt_park();
    }
}

void unparker_task(void *opaque) {
    const auto count = *static_cast<const std::int64_t *>(opaque);
    dlsm_gt_task *task = nullptr;
    while (!(task = parked_task.load(std::memory_order_acquire))) {
        dlsm_gt_yield();
    }
    for (std::int64_t i = 0; i < count; ++i) {
        dlsm_gt_unpark(task);
        dlsm_gt_yield();
    }
}

void BM_ParkUnpark(benchmark::State &state) {
    const std::int64_t count = state.range(0);
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        parked_task.store(nullptr, std::memory_order_relaxed);
        dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
        dlsm_gt_spawn(rt, parker_task, const_cast<std::int64_t *>(&count));
        dlsm_gt_spawn(rt, unparker_task, const_cast<std::int64_t *>(&count));
        state.ResumeTiming();
        dlsm_status status = dlsm_gt_run(rt);
        benchmark::DoNotOptimize(status);
        state.PauseTiming();
        dlsm_gt_runtime_free(rt);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * count);
}

void BM_ParkUnparkCrossVP(benchmark::State &state) {
    const std::int64_t count = state.range(0);
    for (auto _ : state) {
        (void)_;
        state.PauseTiming();
        parked_task.store(nullptr, std::memory_order_relaxed);
        dlsm_gt_runtime *rt = dlsm_gt_runtime_new(2, 0);
        dlsm_gt_task_options parker_options = DLSM_GT_TASK_OPTIONS_INIT;
        parker_options.group_id = DLSM_GT_GROUP_DEFAULT;
        parker_options.vp_id = 0;
        dlsm_gt_task_options unparker_options = parker_options;
        unparker_options.vp_id = 1;
        dlsm_gt_spawn_ex(rt, parker_task,
                         const_cast<std::int64_t *>(&count), &parker_options);
        dlsm_gt_spawn_ex(rt, unparker_task,
                         const_cast<std::int64_t *>(&count), &unparker_options);
        state.ResumeTiming();
        dlsm_status status = dlsm_gt_run(rt);
        benchmark::DoNotOptimize(status);
        state.PauseTiming();
        dlsm_gt_runtime_free(rt);
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * count);
}

void BM_GTMutex(benchmark::State &state, bool contended) {
    const std::int64_t count = state.range(0);
    const int task_count = contended ? 2 : 1;
    for (auto _ : state) {
        (void)_;
        void *mutex = dlsm_bench_gt_mutex_new();
        std::uint64_t counter = 0;
        dlsm_gt_runtime *rt = dlsm_gt_runtime_new(task_count, 0);
        if (!mutex || !rt) {
            if (mutex) { dlsm_bench_gt_mutex_destroy(mutex); }
            dlsm_gt_runtime_free(rt);
            state.SkipWithError("GT mutex benchmark setup failed");
            break;
        }
        std::vector<dlsm_bench_gt_mutex_args> args(
            static_cast<std::size_t>(task_count));
        for (int task = 0; task < task_count; ++task) {
            args[static_cast<std::size_t>(task)] = {
                mutex, count, &counter, contended ? 1 : 0, DLSM_OK};
            dlsm_gt_task_options options = DLSM_GT_TASK_OPTIONS_INIT;
            options.group_id = DLSM_GT_GROUP_DEFAULT;
            options.vp_id = task;
            dlsm_gt_spawn_ex(rt, dlsm_bench_gt_mutex_task,
                             &args[static_cast<std::size_t>(task)], &options);
        }
        dlsm_status run_status = dlsm_gt_run(rt);
        int destroy_status = dlsm_bench_gt_mutex_destroy(mutex);
        benchmark::DoNotOptimize(counter);
        benchmark::DoNotOptimize(run_status);
        benchmark::DoNotOptimize(destroy_status);
        dlsm_gt_runtime_free(rt);
        if (run_status != DLSM_OK || destroy_status != DLSM_OK ||
            counter != static_cast<std::uint64_t>(count * task_count)) {
            state.SkipWithError("GT mutex benchmark failed");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * count * task_count);
}

void BM_GTMutexUncontended(benchmark::State &state) {
    BM_GTMutex(state, false);
}

void BM_GTMutexContended(benchmark::State &state) {
    BM_GTMutex(state, true);
}

struct PThreadMutexArgs {
    pthread_mutex_t *mutex;
    std::int64_t count;
    std::uint64_t *counter;
    bool contended;
};

void *pthread_mutex_task(void *opaque) {
    auto *args = static_cast<PThreadMutexArgs *>(opaque);
    for (std::int64_t i = 0; i < args->count; ++i) {
        pthread_mutex_lock(args->mutex);
        ++*args->counter;
        if (args->contended) { sched_yield(); }
        pthread_mutex_unlock(args->mutex);
    }
    return nullptr;
}

void BM_PThreadMutex(benchmark::State &state, bool contended) {
    const std::int64_t count = state.range(0);
    const int thread_count = contended ? 2 : 1;
    for (auto _ : state) {
        (void)_;
        pthread_mutex_t mutex;
        pthread_mutex_init(&mutex, nullptr);
        std::uint64_t counter = 0;
        PThreadMutexArgs args{&mutex, count, &counter, contended};
        std::vector<pthread_t> threads(static_cast<std::size_t>(thread_count));
        int created = 0;
        for (; created < thread_count; ++created) {
            if (pthread_create(&threads[static_cast<std::size_t>(created)],
                               nullptr, pthread_mutex_task, &args) != 0) {
                break;
            }
        }
        for (int thread = 0; thread < created; ++thread) {
            pthread_join(threads[static_cast<std::size_t>(thread)], nullptr);
        }
        pthread_mutex_destroy(&mutex);
        benchmark::DoNotOptimize(counter);
        if (created != thread_count) {
            state.SkipWithError("pthread mutex thread creation failed");
            break;
        }
        if (counter != static_cast<std::uint64_t>(count * thread_count)) {
            state.SkipWithError("pthread mutex benchmark failed");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * count * thread_count);
}

void BM_PThreadMutexUncontended(benchmark::State &state) {
    BM_PThreadMutex(state, false);
}

void BM_PThreadMutexContended(benchmark::State &state) {
    BM_PThreadMutex(state, true);
}

struct SemaphoreArgs {
    sem_t *wait_on;
    sem_t *post_to;
    std::int64_t count;
};

void *semaphore_ping_pong(void *opaque) {
    const auto *args = static_cast<const SemaphoreArgs *>(opaque);
    for (std::int64_t i = 0; i < args->count; ++i) {
        int status;
        do {
            status = sem_wait(args->wait_on);
        } while (status != 0 && errno == EINTR);
        if (status != 0 || sem_post(args->post_to) != 0) { return nullptr; }
    }
    return nullptr;
}

void BM_PThreadSemaphorePingPong(benchmark::State &state) {
    const std::int64_t count = state.range(0);
    for (auto _ : state) {
        (void)_;
        sem_t first;
        sem_t second;
        sem_init(&first, 0, 1);
        sem_init(&second, 0, 0);
        SemaphoreArgs first_args{&first, &second, count};
        SemaphoreArgs second_args{&second, &first, count};
        pthread_t first_thread;
        pthread_t second_thread;
        pthread_create(&first_thread, nullptr, semaphore_ping_pong, &first_args);
        pthread_create(&second_thread, nullptr, semaphore_ping_pong, &second_args);
        pthread_join(first_thread, nullptr);
        pthread_join(second_thread, nullptr);
        sem_destroy(&first);
        sem_destroy(&second);
    }
    state.SetItemsProcessed(state.iterations() * count);
}

struct ComputeArgs {
    std::uint64_t seed;
    std::uint64_t iterations;
    std::uint64_t result;
};

void compute_task(void *opaque) {
    auto *args = static_cast<ComputeArgs *>(opaque);
    std::uint64_t value = args->seed;
    for (std::uint64_t i = 0; i < args->iterations; ++i) {
        value = value * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    }
    args->result = value;
}

void compute_without_green_yield(ComputeArgs *args) {
    std::uint64_t value = args->seed;
    for (std::uint64_t i = 0; i < args->iterations; ++i) {
        value = value * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    }
    args->result = value;
}

struct PThreadPool {
    pthread_mutex_t mutex;
    pthread_cond_t work_available;
    pthread_cond_t all_done;
    std::vector<pthread_t> threads;
    std::vector<ComputeArgs> *tasks;
    std::size_t next_task;
    std::size_t completed;
    bool batch_ready;
    bool stop;
};

void *pthread_pool_thread(void *opaque) {
    auto *pool = static_cast<PThreadPool *>(opaque);
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->stop &&
               (!pool->batch_ready || pool->next_task >= pool->tasks->size())) {
            pthread_cond_wait(&pool->work_available, &pool->mutex);
        }
        if (pool->stop) {
            pthread_mutex_unlock(&pool->mutex);
            return nullptr;
        }
        const std::size_t index = pool->next_task++;
        pthread_mutex_unlock(&pool->mutex);

        compute_without_green_yield(&(*pool->tasks)[index]);

        pthread_mutex_lock(&pool->mutex);
        ++pool->completed;
        if (pool->completed == pool->tasks->size()) {
            pool->batch_ready = false;
            pthread_cond_signal(&pool->all_done);
        }
        pthread_mutex_unlock(&pool->mutex);
    }
}

bool pthread_pool_init(PThreadPool *pool, int thread_count,
                       std::vector<ComputeArgs> *tasks) {
    pthread_mutex_init(&pool->mutex, nullptr);
    pthread_cond_init(&pool->work_available, nullptr);
    pthread_cond_init(&pool->all_done, nullptr);
    pool->threads.clear();
    pool->threads.reserve(static_cast<std::size_t>(thread_count));
    pool->tasks = tasks;
    pool->next_task = 0;
    pool->completed = 0;
    pool->batch_ready = false;
    pool->stop = false;
    for (int i = 0; i < thread_count; ++i) {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, pthread_pool_thread, pool) != 0) {
            pthread_mutex_lock(&pool->mutex);
            pool->stop = true;
            pthread_cond_broadcast(&pool->work_available);
            pthread_mutex_unlock(&pool->mutex);
            for (pthread_t created : pool->threads) {
                pthread_join(created, nullptr);
            }
            pthread_cond_destroy(&pool->all_done);
            pthread_cond_destroy(&pool->work_available);
            pthread_mutex_destroy(&pool->mutex);
            return false;
        }
        pool->threads.push_back(thread);
    }
    return true;
}

void pthread_pool_run(PThreadPool *pool) {
    pthread_mutex_lock(&pool->mutex);
    pool->next_task = 0;
    pool->completed = 0;
    pool->batch_ready = true;
    pthread_cond_broadcast(&pool->work_available);
    while (pool->batch_ready) {
        pthread_cond_wait(&pool->all_done, &pool->mutex);
    }
    pthread_mutex_unlock(&pool->mutex);
}

void pthread_pool_destroy(PThreadPool *pool) {
    pthread_mutex_lock(&pool->mutex);
    pool->stop = true;
    pthread_cond_broadcast(&pool->work_available);
    pthread_mutex_unlock(&pool->mutex);
    for (pthread_t thread : pool->threads) {
        pthread_join(thread, nullptr);
    }
    pthread_cond_destroy(&pool->all_done);
    pthread_cond_destroy(&pool->work_available);
    pthread_mutex_destroy(&pool->mutex);
}

void BM_VPScaling(benchmark::State &state) {
    const int vp_count = static_cast<int>(state.range(0));
    constexpr int tasks_per_vp = 32;
    constexpr std::uint64_t iterations_per_task = 65536;
    std::vector<ComputeArgs> args(static_cast<std::size_t>(vp_count * tasks_per_vp));
    std::uint64_t total_cpu_ns = 0;
    std::uint64_t total_wall_ns = 0;
    std::uint64_t total_sleeps = 0;
    std::uint64_t total_os_wakeups = 0;
    std::uint64_t total_spin_wakeups = 0;
    std::uint64_t total_migrations = 0;
    for (auto _ : state) {
        (void)_;
        dlsm_gt_runtime *rt = dlsm_gt_runtime_new(vp_count, 0);
        for (std::size_t i = 0; i < args.size(); ++i) {
            args[i] = {i + 1, iterations_per_task, 0};
            dlsm_gt_spawn(rt, compute_task, &args[i]);
        }
        dlsm_status status = dlsm_gt_run(rt);
        benchmark::DoNotOptimize(status);
        for (auto &arg : args) { benchmark::DoNotOptimize(arg.result); }
        for (int vp = 0; vp < vp_count; ++vp) {
            dlsm_gt_vp_stats stats;
            if (dlsm_gt_runtime_vp_stats(rt, vp, &stats) == DLSM_OK) {
                total_cpu_ns += stats.thread_cpu_ns;
                total_wall_ns += stats.wall_ns;
                total_sleeps += stats.sleep_count;
                total_os_wakeups += stats.os_wakeups;
                total_spin_wakeups += stats.spin_wakeups;
                total_migrations += stats.migrations;
            }
        }
        dlsm_gt_runtime_free(rt);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(args.size()));
    const double measured_iterations = static_cast<double>(state.iterations());
    state.counters["cpu_util_%"] = total_wall_ns == 0 ? 0.0
        : 100.0 * static_cast<double>(total_cpu_ns) /
              static_cast<double>(total_wall_ns);
    state.counters["os_wakeups/iter"] = total_os_wakeups / measured_iterations;
    state.counters["sleeps/iter"] = total_sleeps / measured_iterations;
    state.counters["spin_wakeups/iter"] = total_spin_wakeups / measured_iterations;
    state.counters["migrations/iter"] = total_migrations / measured_iterations;
}

void BM_PThreadPoolScaling(benchmark::State &state) {
    const int thread_count = static_cast<int>(state.range(0));
    constexpr int tasks_per_thread = 32;
    constexpr std::uint64_t iterations_per_task = 65536;
    std::vector<ComputeArgs> args(static_cast<std::size_t>(thread_count * tasks_per_thread));
    for (auto _ : state) {
        (void)_;
        for (std::size_t i = 0; i < args.size(); ++i) {
            args[i] = {i + 1, iterations_per_task, 0};
        }
        PThreadPool pool{};
        if (!pthread_pool_init(&pool, thread_count, &args)) {
            state.SkipWithError("pthread pool thread creation failed");
            break;
        }
        pthread_pool_run(&pool);
        for (auto &arg : args) { benchmark::DoNotOptimize(arg.result); }
        pthread_pool_destroy(&pool);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(args.size()));
}

struct TimerLatenessArgs {
    std::int64_t count;
    std::uint64_t interval_ns;
    std::vector<std::uint64_t> *samples;
    dlsm_status status;
};

void timer_lateness_task(void *opaque) {
    auto *args = static_cast<TimerLatenessArgs *>(opaque);
    std::uint64_t deadline = dlsm_gt_now();
    if (deadline == 0) {
        args->status = DLSM_GT_E_WAIT;
    } else {
        args->status = DLSM_OK;
    }
    for (std::int64_t i = 0; i < args->count && args->status == DLSM_OK; ++i) {
        deadline = UINT64_MAX - deadline < args->interval_ns
            ? UINT64_MAX : deadline + args->interval_ns;
        args->status = dlsm_gt_sleep_until(deadline);
        const std::uint64_t resumed = dlsm_gt_now();
        if (args->status == DLSM_OK && resumed >= deadline) {
            args->samples->push_back(resumed - deadline);
        }
    }
}

double percentile(const std::vector<std::uint64_t> &sorted, double fraction) {
    if (sorted.empty()) { return 0.0; }
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[index]);
}

void BM_TimerLateness(benchmark::State &state) {
    const std::int64_t timer_count = state.range(0);
    constexpr std::uint64_t interval_ns = UINT64_C(100000);
    std::vector<std::uint64_t> samples;
    std::uint64_t detection_total = 0;
    std::uint64_t detection_max = 0;
    std::uint64_t ready_total = 0;
    std::uint64_t ready_max = 0;
    std::uint64_t resume_total = 0;
    std::uint64_t resume_max = 0;
    std::uint64_t expired = 0;
    bool failed = false;
    for (auto _ : state) {
        (void)_;
        dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
        TimerLatenessArgs args{timer_count, interval_ns, &samples,
                              DLSM_GT_E_STATE};
        dlsm_gt_spawn(rt, timer_lateness_task, &args);
        dlsm_status status = dlsm_gt_run(rt);
        benchmark::DoNotOptimize(status);
        if (status != DLSM_OK || args.status != DLSM_OK) {
            state.SkipWithError("green-thread timer wait failed");
            failed = true;
        }
        dlsm_gt_stats stats{};
        if (dlsm_gt_runtime_stats(rt, &stats) == DLSM_OK) {
            detection_total += stats.timer_detection_lateness_ns_total;
            detection_max = std::max(detection_max,
                                     stats.timer_detection_lateness_ns_max);
            ready_total += stats.timer_ready_lateness_ns_total;
            ready_max = std::max(ready_max,
                                 stats.timer_ready_lateness_ns_max);
            resume_total += stats.timer_resume_lateness_ns_total;
            resume_max = std::max(resume_max,
                                  stats.timer_resume_lateness_ns_max);
            expired += stats.timers_expired;
        }
        dlsm_gt_runtime_free(rt);
        if (failed) { break; }
    }
    std::sort(samples.begin(), samples.end());
    long double sum = 0;
    for (std::uint64_t sample : samples) { sum += sample; }
    state.counters["lateness_avg_ns"] = samples.empty()
        ? 0.0 : static_cast<double>(sum / samples.size());
    state.counters["lateness_p50_ns"] = percentile(samples, 0.50);
    state.counters["lateness_p95_ns"] = percentile(samples, 0.95);
    state.counters["lateness_p99_ns"] = percentile(samples, 0.99);
    state.counters["lateness_max_ns"] = samples.empty()
        ? 0.0 : static_cast<double>(samples.back());
    state.counters["detect_avg_ns"] = expired == 0 ? 0.0
        : static_cast<double>(detection_total) / static_cast<double>(expired);
    state.counters["detect_max_ns"] = static_cast<double>(detection_max);
    state.counters["ready_avg_ns"] = expired == 0 ? 0.0
        : static_cast<double>(ready_total) / static_cast<double>(expired);
    state.counters["ready_max_ns"] = static_cast<double>(ready_max);
    state.counters["resume_avg_ns"] = expired == 0 ? 0.0
        : static_cast<double>(resume_total) / static_cast<double>(expired);
    state.counters["resume_max_ns"] = static_cast<double>(resume_max);
    state.SetItemsProcessed(static_cast<std::int64_t>(samples.size()));
}

struct TimerFanoutArgs {
    std::uint64_t duration_ns;
    dlsm_status status;
};

void timer_fanout_task(void *opaque) {
    auto *args = static_cast<TimerFanoutArgs *>(opaque);
    args->status = dlsm_gt_sleep_for(args->duration_ns);
}

void BM_TimerFanout(benchmark::State &state) {
    const int timer_count = static_cast<int>(state.range(0));
    constexpr std::uint64_t duration_ns = UINT64_C(20000000);
    std::vector<TimerFanoutArgs> args(static_cast<std::size_t>(timer_count));
    for (auto _ : state) {
        (void)_;
        dlsm_gt_runtime *rt = dlsm_gt_runtime_new(1, 0);
        bool submit_failed = false;
        for (TimerFanoutArgs &arg : args) {
            arg = {duration_ns, DLSM_GT_E_STATE};
            if (!dlsm_gt_spawn(rt, timer_fanout_task, &arg)) {
                submit_failed = true;
                break;
            }
        }
        const dlsm_status run_status = dlsm_gt_run(rt);
        bool wait_failed = false;
        for (const TimerFanoutArgs &arg : args) {
            if (arg.status != DLSM_OK) {
                wait_failed = true;
                break;
            }
        }
        dlsm_gt_stats stats{};
        dlsm_gt_runtime_stats(rt, &stats);
        benchmark::DoNotOptimize(stats.timers_expired);
        dlsm_gt_runtime_free(rt);
        if (submit_failed || run_status != DLSM_OK || wait_failed ||
            stats.timers_expired != static_cast<std::uint64_t>(timer_count)) {
            state.SkipWithError("timer fan-out benchmark failed");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * timer_count);
}

BENCHMARK(BM_ContextSwitch)->Name("GT/CooperativeYield")->Arg(100000)->UseRealTime();
BENCHMARK(BM_PThreadSchedYield)
    ->Name("PThread/SchedYield")->Arg(100000)->UseRealTime();
BENCHMARK(BM_ParkUnpark)->Name("GT/ParkUnpark")->Arg(100000)->UseRealTime();
BENCHMARK(BM_ParkUnparkCrossVP)
    ->Name("GT/ParkUnparkCrossVP")->Arg(100000)->UseRealTime();
BENCHMARK(BM_PThreadSemaphorePingPong)
    ->Name("PThread/SemaphorePingPong")->Arg(100000)->UseRealTime();

BENCHMARK(BM_SpawnAndFinish)
    ->Name("GT/SpawnAndFinish")->Arg(64)->Arg(256)->Arg(1024)->UseRealTime();
BENCHMARK(BM_SpawnDetachedAndReclaim)
    ->Name("GT/SpawnDetachedFinishReclaim")
    ->Arg(64)->Arg(256)->Arg(1024)->UseRealTime();
BENCHMARK(BM_PThreadCreateJoin)
    ->Name("PThread/CreateJoin")->Arg(64)->Arg(256)->Arg(1024)->UseRealTime();

BENCHMARK(BM_VPScaling)
    ->Name("GT/VPScaling")->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime();
BENCHMARK(BM_PThreadPoolScaling)
    ->Name("PThreadPool/ThreadScaling")->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime();
BENCHMARK(BM_TimerLateness)
    ->Name("GT/TimerLateness")->Arg(64)->Arg(256)->UseRealTime();
BENCHMARK(BM_TimerFanout)
    ->Name("GT/TimerFanout")->Arg(64)->Arg(256)->UseRealTime();
BENCHMARK(BM_GTMutexUncontended)
    ->Name("GT/MutexUncontended")->Arg(100000)->UseRealTime();
BENCHMARK(BM_PThreadMutexUncontended)
    ->Name("PThread/MutexUncontended")->Arg(100000)->UseRealTime();
BENCHMARK(BM_GTMutexContended)
    ->Name("GT/MutexContended")->Arg(100000)->UseRealTime();
BENCHMARK(BM_PThreadMutexContended)
    ->Name("PThread/MutexContended")->Arg(100000)->UseRealTime();

} // namespace

BENCHMARK_MAIN();
