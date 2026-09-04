#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>

#ifndef SCHEDULER_CASE
#define SCHEDULER_CASE cases/qwen3_14b_decode_subgraph.h
#endif

/* Macro to stringify the include directive properly */
#define __INCLUDE(x) #x
#define _INCLUDE_FILE(x) __INCLUDE(x)
#include _INCLUDE_FILE(SCHEDULER_CASE)

#include "painter.h"
#include "log.h"

static inline uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

uint32_t commit_task_id[PAINTER_THREAD_CNT] = {0, 0};

extern cacheline_bool_t g_is_done;
uint32_t completed_task_cnt = 0;

void send_2_ready_queue(queue_t *queue, uint32_t ready_cnt[], uint32_t rq_buf[][RQ_BATCH_SIZE]) {
    for (uint32_t j = 0; j < TASK_TYPE_CNT; j++) {
        if (ready_cnt[j] > 0)
        {
            WORKER_LOGF("batch_enqueue,%u,cnt,%u,first,%u", j, ready_cnt[j], rq_buf[j][0]);
            batch_enqueue(&queue[j], rq_buf[j], ready_cnt[j]);
        }
    }
}

void init_queue(void)
{
    for (uint32_t i = 0; i < total_task_cnt; i++) {
        uint32_t task_id = total_task_id[i];
        int pre_cnt = test_graph[0].total_pre_cnt[task_id];
        if (pre_cnt > 1) continue;

        queue_t *queue = (pre_cnt == 0) ? g_ready_queue : g_near_ready_queue;
        task_type_t type = (task_type_t)total_type[task_id];
        total_task_state[task_id] = 1;
        uint32_t idx = (uint32_t)atomic_fetch_add_explicit(&queue[type].tail, 1, memory_order_relaxed);
        queue[type].tasks[idx] = task_id;
    }
}

void resolve_dep(int tid, uint32_t cnt, const uint32_t* cq_buf,
                 uint32_t rq_buf[][RQ_BATCH_SIZE], uint32_t* ready_cnt,
                 uint32_t nrq_buf[][RQ_BATCH_SIZE], uint32_t* near_cnt) {
    uint32_t task_id;
    uint32_t succ_id;
    uint32_t succ_cnt;
    uint32_t idx;

    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        succ_cnt = (uint32_t)test_graph[tid].suc_cnt[task_id];
        if (succ_cnt <= 0) continue;
        
        idx = test_graph[tid].suc_idx[task_id];
        for (uint32_t k = idx; k < (idx + succ_cnt); k++) {
            succ_id = test_graph[tid].successors[k];
            if (total_task_state[succ_id] == 1)
                continue;
            test_graph[tid].total_pre_cnt[succ_id]--;
            total_pred_xor[succ_id] ^= task_id;
            WORKER_LOGF("painter,task_id,%u,successor_id,%u,predecessor_cnt,%d", task_id, succ_id, test_graph[tid].total_pre_cnt[succ_id]);
            if (test_graph[tid].total_pre_cnt[succ_id] < 1) {
                task_type_t type = total_type[succ_id];
                rq_buf[type][ready_cnt[type]] = succ_id;
                ready_cnt[type]++;
                WORKER_LOGF("ready,task_id,%u,type,%d,cnt,%u", succ_id, type, ready_cnt[type]);
            }
            else if (test_graph[tid].total_pre_cnt[succ_id] == 1) {
                /* One predecessor left, so the task can already be placed on a
                 * unit and wait there. Marking it queued takes it out of
                 * dependency tracking for good, which also freezes total_pred_xor -
                 * the id of the predecessor it is waiting for. */
                task_type_t type = total_type[succ_id];
                total_task_state[succ_id] = 1;
                nrq_buf[type][near_cnt[type]] = succ_id;
                near_cnt[type]++;
                WORKER_LOGF("near_ready,task_id,%u,pred,%u,type,%d", succ_id, total_pred_xor[succ_id], type);
            }
        }
    }
}

void deal_completed_queue(int tid) {
    uint32_t rq_buf[TASK_TYPE_CNT][RQ_BATCH_SIZE];
    uint32_t ready_cnt[TASK_TYPE_CNT] = {0, 0};
    uint32_t nrq_buf[TASK_TYPE_CNT][RQ_BATCH_SIZE];
    uint32_t near_cnt[TASK_TYPE_CNT] = {0, 0};

    /* Shared completed queue: every painter drains the same ring
     * independently via its own read cursor. Zero-copy: grab a batch straight
     * from the ring without copying, then advance the cursor after consuming. */
    uint32_t cnt = 0;
    const uint32_t *cq_buf = completed_queue_read_batch(&g_completed_queue, tid, &cnt, CQ_BATCH_SIZE);
    if (cnt <= 0)
        return;

    if (tid == 0)
    {
        completed_task_cnt += cnt;
        if(completed_task_cnt >= total_task_cnt) {
            atomic_store_explicit(&g_is_done.v, true, memory_order_release);
        }
    }
    
    resolve_dep(tid, cnt, cq_buf, rq_buf, ready_cnt, nrq_buf, near_cnt);
    /* Batch consumed: move this painter's read cursor forward. */
    completed_queue_read_commit(&g_completed_queue, tid, cnt);
    send_2_ready_queue(g_ready_queue, ready_cnt, rq_buf);
    send_2_ready_queue(g_near_ready_queue, near_cnt, nrq_buf);
}

void *painter(void *arg)
{
    int tid = (int)(intptr_t)arg;
    int cnt = atomic_fetch_add(&g_start_barrier.v, 1);
    while (cnt < (DISPATCH_THREAD_CNT + PAINTER_THREAD_CNT))
    {
        cnt = atomic_load(&g_start_barrier.v);
    }
    
    uint64_t start_ns = get_time_ns();
    bool is_done = false;
    while (!is_done) {
        deal_completed_queue(tid);
        is_done = atomic_load(&g_is_done.v);
    }
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;
    if (tid == 0)
    {
        printf("scheduler_throughput,%.2f,MTasks/s\n",(float)(total_task_cnt * 1000.0 / elapsed_ns));
    }
    WORKER_LOGF("painter,%d,done", tid);
    return NULL;
}