/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */
#include <stdint.h>
#include <stdio.h>

#include "dispatch.h"
#include "task.h"
#include "log.h"
#include "a6.h"

extern cacheline_bool_t g_is_done;

ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];

/* Global, thread-shared queues (single instance for the whole scheduler). */
queue_t g_ready_queue[TASK_TYPE_CNT];
queue_t g_near_ready_queue[TASK_TYPE_CNT];
completed_queue_t g_completed_queue;

void init_global_queues(void)
{
    for (int i = 0; i < TASK_TYPE_CNT; i++) {
        memset(&g_ready_queue[i], 0, sizeof(queue_t));
        atomic_flag_clear_explicit(&g_ready_queue[i].head_lock, memory_order_release);
        atomic_flag_clear_explicit(&g_ready_queue[i].tail_lock, memory_order_release);

        memset(&g_near_ready_queue[i], 0, sizeof(queue_t));
        atomic_flag_clear_explicit(&g_near_ready_queue[i].head_lock, memory_order_release);
        atomic_flag_clear_explicit(&g_near_ready_queue[i].tail_lock, memory_order_release);
    }

    memset(&g_completed_queue, 0, sizeof(completed_queue_t));
    atomic_flag_clear_explicit(&g_completed_queue.write_lock.v, memory_order_release);
    atomic_store_explicit(&g_completed_queue.write_pos.v, 0, memory_order_release);
    for (int p = 0; p < PAINTER_THREAD_CNT; p++) {
        atomic_store_explicit(&g_completed_queue.read_pos[p].v, 0, memory_order_release);
    }
}

void init_ctrl_t(void)
{
    for (int tid = 0; tid < DISPATCH_THREAD_CNT; tid++) {
        g_ctrl_t[tid].tid = (uint32_t)tid;
        if (AIC_CNT_PER_THREAD >= 64) {
            g_ctrl_t[tid].aicore_mask = ~0ULL;
        } else {
            g_ctrl_t[tid].aicore_mask = ~0ULL >> (64 - AIC_CNT_PER_THREAD);
        }

        // Initialize free_bitmap for TASK_TYPE
        for (int i = 0; i < TASK_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                g_ctrl_t[tid].free_bitmap[i][j] = g_ctrl_t[tid].aicore_mask;
            }
        }
        // set_mix(tid);
        // Initialize msg_bitmap for EXE_TYPE
        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_OSTD; j++) {
                g_ctrl_t[tid].msg_bitmap[i][j] = 0x0;
            }
        }
        
        // Init task_id_map
        for (int i = 0; i < EXE_TYPE_CNT; i++) {
            for (int j = 0; j < AIC_CNT; j++) {
                g_ctrl_t[tid].task_id_map1[i][j] = 0;
                g_ctrl_t[tid].task_id_map2[i][j] = 0;
            }
        }

        // Init aicore_spr
        uint64_t base = 0;
        uint64_t idx = 0;
        for (size_t i = 0; i < EXE_TYPE_CNT; i++)
        {
            idx = 0;
            for (size_t j = AIC_CNT_PER_THREAD * tid; j < AIC_CNT_PER_THREAD * (tid + 1); j++)
            {
                base = AICORE_SPR_BASE;
                base += (i == 0 ? AICORE_CUBE_OFFSET : AICORE_VECTOR_OFFSET);
                if (j >= AIC_CNT_PER_DIE) {
                    base += AICORE_DIE_OFFSET + AICORE_OFFSET * (j - AIC_CNT_PER_DIE);
                } else {
                    base += AICORE_OFFSET * j;
                }

                g_ctrl_t[tid].aicore_spr_1[i][idx] = (uint64_t*)base;
                g_ctrl_t[tid].aicore_spr_2[i][idx] = (uint64_t*)(base + AICORE_SPR_OFFSET); 
                idx++;
            }
        }
        
    }
}

static inline void set_mix(int tid)
{
    for (int j = 0; j < AIC_OSTD; j++) {
        g_ctrl_t[tid].free_bitmap[TASK_TYPE_MIX][j] =
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_CUBE][j] &
            g_ctrl_t[tid].free_bitmap[TASK_TYPE_VECTOR][j];
    }
}

static void hand_shake(int cpu_idx, uint64_t* aicore_spr[], int type, int ostd2_offset) {
    uint64_t base = AICPU_MSGQ_BASE + cpu_idx * AICPU_OFFSET + ostd2_offset * AICPU_MSGQ_OFFSET;
    uint64_t msgq_addr = 0;
    (void)aicore_spr;
    (void)msgq_addr;

    for (size_t i = 0; i < AIC_CNT_PER_THREAD; i++)
    {
        uint64_t offset = type == 0 ? 0 : 128;
        msgq_addr = base + (i + offset)  * AICPU_MSGQ_OFFSET;
        #ifdef REAL_CHIP
        *aicore_spr[i] = HAND_SHAKE_VAL | (msgq_addr & LOAW_ADDR_MASK);
        #endif
        WORKER_LOGF("cpu_idx,%d,index,%zu,aicore_spr,%p,msgq_addr,%llx", cpu_idx, i, (void*)aicore_spr[i], (unsigned long long)msgq_addr);
    }
}

static inline void read_msgq(int tid)
{
    #ifdef REAL_CHIP
    uint64_t msgq_value[4];
    READ_REG(g_ctrl_t[tid].msg_bitmap[0][0], MSGQ_VLD0);
    WRITE_REG(MSGQ_VLD0, g_ctrl_t[tid].msg_bitmap[0][0]);

    READ_REG(g_ctrl_t[tid].msg_bitmap[0][1], MSGQ_VLD1);
    WRITE_REG(MSGQ_VLD1, g_ctrl_t[tid].msg_bitmap[0][1]);

    READ_REG(g_ctrl_t[tid].msg_bitmap[1][0], MSGQ_VLD2);
    WRITE_REG(MSGQ_VLD2, g_ctrl_t[tid].msg_bitmap[1][0]);

    READ_REG(g_ctrl_t[tid].msg_bitmap[1][1], MSGQ_VLD3);
    WRITE_REG(MSGQ_VLD3, g_ctrl_t[tid].msg_bitmap[1][1]);
    #endif

    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        for (int j = 0; j < AIC_OSTD; j++) {
            g_ctrl_t[tid].free_bitmap[i][j] |= g_ctrl_t[tid].msg_bitmap[i][j];
        }
    }
    set_mix(tid);
}

static inline void get_completed(uint64_t* bitmap, uint32_t task_id[], int *complete_cnt,
                                 const uint32_t task_id_map[])
{
    int cnt = __builtin_popcountll(*bitmap);
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(*bitmap);
        task_id[(*complete_cnt)] = task_id_map[idx];
        WORKER_LOGF("completed,task_id,%u,complete_cnt,%d,core,%llu,bitmap,%llu", task_id_map[idx], *complete_cnt, (unsigned long long)idx, (unsigned long long)*bitmap);
        (*complete_cnt)++;
        cnt--;
        *bitmap &= (*bitmap - 1);
    }
}

static inline void push_2_completed_queue(int tid)
{
    uint32_t task_id[240];
    int complete_cnt = 0;
    for (int i = 0; i < EXE_TYPE_CNT; i++) {
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][0], task_id, &complete_cnt, g_ctrl_t[tid].task_id_map1[i]);
        get_completed(&g_ctrl_t[tid].msg_bitmap[i][1], task_id, &complete_cnt, g_ctrl_t[tid].task_id_map2[i]);
    }
    completed_queue_write_batch(&g_completed_queue, task_id, (uint32_t)complete_cnt);
}

static inline int send_task(ctrl_t *ctrl, queue_t *queue, int type, bool *has_idle_slot)
{
    int sent = 0;

    for (int slot = 0; slot < AIC_OSTD; slot++) {
        uint64_t free_bitmap = ctrl->free_bitmap[type][slot] & ctrl->aicore_mask;
        int free_demand = __builtin_popcountll(free_bitmap);
        if (free_demand <= 0) continue;

        uint32_t task_ids[AIC_CNT];
        uint32_t got = (uint32_t)free_demand;
        if (!batch_dequeue(queue, task_ids, &got)) {
            break;
        }

        for (uint32_t i = 0; i < got; i++) {
            uint32_t task_id = task_ids[i];
            uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
            uint64_t mask = (uint64_t)0x1 << idx;
            int core = (int)idx;
            (void)core;

            if (slot == 1) {
                ctrl->task_id_map2[type][idx] = task_id;
                #ifdef REAL_CHIP
                *ctrl->aicore_spr_2[type][idx] = task_id;
                #endif
            } else {
                ctrl->task_id_map1[type][idx] = task_id;
                #ifdef REAL_CHIP
                *ctrl->aicore_spr_1[type][idx] = task_id;
                #endif
            }

            // Clear the free bit for this core/slot combination (mark as busy)
            ctrl->free_bitmap[type][slot] &= ~mask;

            #ifndef REAL_CHIP
            ctrl->msg_bitmap[type][slot] |= mask;
            #endif

            WORKER_LOGF("send,task_id,%u,core,%d,slot,%d,type,%d", task_id, core, slot, type);
            sent++;
            free_bitmap &= ~mask;
        }
    }

    /* Report whether any slot of this type is still free, so the caller can
     * decide whether to drain the near-ready queue. Only ever set to true:
     * the caller initialises it to false and may share it across calls. */
    if (has_idle_slot != NULL) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            if (ctrl->free_bitmap[type][slot] & ctrl->aicore_mask) {
                *has_idle_slot = true;
                break;
            }
        }
    }

    return sent;
}

int dispatch(int tid)
{
    int total_sent = 0;
    bool has_idle_slot = false;

    read_msgq(tid);
    push_2_completed_queue(tid);
    // total_sent += send_task(&g_ctrl_t[tid], &g_ready_queue[TASK_TYPE_MIX], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], &g_ready_queue[TASK_TYPE_VECTOR], TASK_TYPE_VECTOR, &has_idle_slot);
    total_sent += send_task(&g_ctrl_t[tid], &g_ready_queue[TASK_TYPE_CUBE], TASK_TYPE_CUBE, &has_idle_slot);

    if (has_idle_slot) {
        total_sent += send_task(&g_ctrl_t[tid], &g_near_ready_queue[TASK_TYPE_VECTOR], TASK_TYPE_VECTOR, NULL);
        total_sent += send_task(&g_ctrl_t[tid], &g_near_ready_queue[TASK_TYPE_CUBE], TASK_TYPE_CUBE, NULL);
    }
    return total_sent;
}

/*
 * Dispatch worker thread entry point Runs the dispatch loop for task distribution
 */
void *dispatch_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    int total_sent = 0;

    for (size_t i = 0; i < EXE_TYPE_CNT; i++)
    {        
        hand_shake(tid, g_ctrl_t[tid].aicore_spr_1[i], i, 0);
        hand_shake(tid, g_ctrl_t[tid].aicore_spr_2[i], i, 64);
    }

    int cnt = atomic_fetch_add(&g_start_barrier.v, 1);

    while (cnt < (DISPATCH_THREAD_CNT + PAINTER_THREAD_CNT))
    {
        cnt = atomic_load(&g_start_barrier.v);
    }

    bool is_done = false;
    while (!is_done) {
        total_sent += dispatch(tid);
        is_done = atomic_load(&g_is_done.v);
    }
    WORKER_LOGF("dispatch,%d,done", tid);
    return NULL;
}
