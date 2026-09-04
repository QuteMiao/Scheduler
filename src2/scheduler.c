#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdatomic.h>

#include "cases2/qwen3_14b_decode.h"
#include "hw_queue.h"

#define SCHEDULER_THREAD_CNT 1
/* 每个调度线程负责的 cluster 数量 */
#define CLUSTER_NUM_PER_THREAD (CLUSTER_NUM / SCHEDULER_THREAD_CNT)

#define get_ipc_reg_0() 0
#define get_ipc_reg_1() 1
#define get_ipc_reg_2() 2
#define get_ipc_reg_3() 3
#define get_ipc_reg_4() 4
#define get_ipc_reg_5() 5
#define get_ipc_reg_6() 6
#define get_ipc_reg_7() 7

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

extern cacheline_bool_t g_is_done;
uint32_t completed_task_cnt = 0;


void resolve_dep(uint32_t task_id, int cluster_id) {
    uint32_t succ_cnt = (uint32_t)task_suc_cnt[task_id];
    if (succ_cnt <= 0) return;
    
    uint32_t idx = (uint32_t)task_suc_idx[task_id];
    for (uint32_t k = idx; k < (idx + succ_cnt); k++) {
        uint32_t succ_id = task_successors[k];
        if (atomic_fetch_sub(&task_pre_cnt[succ_id], 1) == 1) {
            task_type_t type = task_type[succ_id];
            queue_push_by_cluster((uint32_t)cluster_id, type, succ_id);
        }
    }
}

void deal_completed_queue(int start_idx, int end_idx) {
    for (int cluster_id = start_idx; cluster_id < end_idx; cluster_id++) {
        uint64_t task_id;
        while (cluster_queue_pop(&g_cluster_complete_queue[cluster_id], &task_id)) {
            resolve_dep((uint32_t)task_id, cluster_id);
        }
    }
}

void *scheduler(void *arg)
{
    int tid = (int)(intptr_t)arg;
    int cnt = atomic_fetch_add(&g_start_barrier.v, 1);
    
    int cluster_start_idx = tid * CLUSTER_NUM_PER_THREAD;
    int cluster_end_idx = cluster_start_idx + CLUSTER_NUM_PER_THREAD;
    cluster_end_idx = cluster_end_idx > CLUSTER_NUM ? CLUSTER_NUM : cluster_end_idx;

    while (cnt < SCHEDULER_THREAD_CNT)
        cnt = atomic_load(&g_start_barrier.v);
    
    uint64_t start_ns = get_time_ns();
    bool is_done = false;
    while (!is_done) {
        deal_completed_queue(cluster_start_idx, cluster_end_idx);
        is_done = atomic_load(&g_is_done.v);
    }
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;
    if (tid == 0)
    {
        printf("scheduler_throughput,%.2f,MTasks/s\n",(float)(total_task_cnt * 1000.0 / elapsed_ns));
    }
    return NULL;
}