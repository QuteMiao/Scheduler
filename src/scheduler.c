#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef __linux__
#include <sched.h>
#endif

#include "conf.h"
#include "painter.h"
#include "dispatch.h"
#include "log.h"
#include <string.h>

uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Global variable definitions needed by dispatch.c and painter.c.
 * Each hot shared atomic owns a private 64-byte cache line (see conf.h). */
cacheline_bool_t g_is_done = { false };
cacheline_int_t g_start_barrier = { 0 };

#define TASK_EXEC_RECORDS_MAX 8192

typedef struct {
    uint32_t task_id;
    uint32_t task_type;      /* TASK_TYPE_CUBE=0, TASK_TYPE_VECTOR=1 */
    uint32_t core_id;        /* local core index within dispatcher (0..AIC_CNT_PER_THREAD-1) */
    uint64_t start_time_ns;
    uint64_t end_time_ns;
} task_exec_record_t;

/* Task execution records — see dispatch.h for the struct definition */
task_exec_record_t g_task_exec_records[TASK_EXEC_RECORDS_MAX];

static void handle_signal(int sig)
{
    (void)sig;
    atomic_store_explicit(&g_is_done.v, true, memory_order_release);
}

int main(void) {
    pthread_t dispatch_threads[DISPATCH_THREAD_CNT];
    pthread_t painter_threads[PAINTER_THREAD_CNT];

#if WORKER_LOG
    log_init("scheduler");
#endif

    init_global_queues();
    init_ctrl_t();
    init_pred_xor();
    init_queue();
    WORKER_LOGF("painter_cnt,%d,dispatcher_cnt,%d", PAINTER_THREAD_CNT, DISPATCH_THREAD_CNT);
    /* Register signal handlers for graceful shutdown on Ctrl+C */
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    for (int i = 0; i < PAINTER_THREAD_CNT; i++) {
        pthread_create(&painter_threads[i], NULL, painter, (void *)(intptr_t)i);
#ifdef __linux__
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET((3 + i), &mask);
        pthread_setaffinity_np(painter_threads[i], sizeof(cpu_set_t), &mask);
#endif
    }

    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_create(&dispatch_threads[i], NULL, dispatch_worker, (void *)(intptr_t)i);
#ifdef __linux__
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(2, &mask);
        pthread_setaffinity_np(dispatch_threads[i], sizeof(cpu_set_t), &mask);
#endif
    }

    for (int i = 0; i < PAINTER_THREAD_CNT; i++) {
        pthread_join(painter_threads[i], NULL);
    }

    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        pthread_join(dispatch_threads[i], NULL);
    }

#if WORKER_LOG
    log_close();
#endif
    return 0;
}
