/*
 * dispatch.c - Dispatch Worker Thread Implementation
 *
 * Worker thread entry point for Dispatch.
 * This file is compiled separately as it contains pthread-specific code.
 */
#include <stdint.h>
#include <stdio.h>

#include "dispatch.h"
#include "painter.h"
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

/* Per-task runtime-state bitmap (three independent bit-planes).
 * One uint64_t word covers 64 task ids per plane:
 *   - NEAR_READY: waiting on its last predecessor (came from g_near_ready_queue)
 *   - DISPATCHED: handed to an AICORE slot (kept set after completion so the
 *                 near-ready lookahead can still find completed predecessors)
 *   - COMPLETED:  its completion has been collected from msg_bitmap
 * Sized to the largest generated workload (3840 tasks). */
#define TASK_STATE_BITMAP_WORDS 80 /* 80 * 64 = 5120 bits */

typedef enum {
    TASK_STATE_NEAR_READY = 0,
    TASK_STATE_DISPATCHED = 1,
    TASK_STATE_COMPLETED  = 2,
    TASK_STATE_PLANE_CNT  = 3,
} task_state_plane_t;

uint64_t g_task_state_bitmap[TASK_STATE_PLANE_CNT][TASK_STATE_BITMAP_WORDS];

static inline uint32_t task_bit_word(uint32_t task_id)
{
    return task_id >> 6;
}

static inline uint64_t task_bit_mask(uint32_t task_id)
{
    return (uint64_t)0x1 << (task_id & 63);
}

static inline bool task_state_get(uint32_t task_id, task_state_plane_t plane)
{
    return (g_task_state_bitmap[plane][task_bit_word(task_id)] & task_bit_mask(task_id)) != 0;
}

static inline void task_state_set(uint32_t task_id, task_state_plane_t plane)
{
    __atomic_fetch_or(&g_task_state_bitmap[plane][task_bit_word(task_id)],
                      task_bit_mask(task_id), __ATOMIC_ACQ_REL);
}

static inline void task_state_clear(uint32_t task_id, task_state_plane_t plane)
{
    __atomic_fetch_and(&g_task_state_bitmap[plane][task_bit_word(task_id)],
                       ~task_bit_mask(task_id), __ATOMIC_ACQ_REL);
}

static inline bool task_is_near_ready(uint32_t task_id)
{
    return task_state_get(task_id, TASK_STATE_NEAR_READY);
}

static inline bool task_is_dispatched(uint32_t task_id)
{
    return task_state_get(task_id, TASK_STATE_DISPATCHED);
}

static inline bool task_is_completed(uint32_t task_id)
{
    return task_state_get(task_id, TASK_STATE_COMPLETED);
}

/* Claim a task as dispatched exactly once. Returns false if another dispatch
 * thread already set the DISPATCHED bit. Also clears NEAR_READY so the task is
 * not picked up again as a near-ready successor. */
static inline bool task_try_dispatch(uint32_t task_id)
{
    uint32_t word = task_bit_word(task_id);
    uint64_t mask = task_bit_mask(task_id);
    uint64_t *dispatched = &g_task_state_bitmap[TASK_STATE_DISPATCHED][word];

    uint64_t old = __atomic_fetch_or(dispatched, mask, __ATOMIC_ACQ_REL);
    if (old & mask) {
        return false;
    }

    __atomic_fetch_and(&g_task_state_bitmap[TASK_STATE_NEAR_READY][word],
                       ~mask, __ATOMIC_ACQ_REL);
    return true;
}

/* Mark a task dispatched after ownership has already been established (e.g. it
 * was dequeued from the shared ready queue by this thread). */
static inline void task_mark_dispatched(uint32_t task_id)
{
    task_state_set(task_id, TASK_STATE_DISPATCHED);
    task_state_clear(task_id, TASK_STATE_NEAR_READY);
}

static inline void task_mark_completed(uint32_t task_id)
{
    task_state_set(task_id, TASK_STATE_COMPLETED);
}

static inline void get_completed(uint64_t* bitmap, uint32_t task_id[], int *complete_cnt,
                                 const uint32_t task_id_map[])
{
    int cnt = __builtin_popcountll(*bitmap);
    while (cnt > 0) {
        uint64_t idx = (uint64_t)__builtin_ctzll(*bitmap);
        task_id[(*complete_cnt)] = task_id_map[idx];
        task_mark_completed(task_id_map[idx]);
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

/* Read (without consuming) g_near_ready_queue and mark every task found there
 * as NEAR_READY. The queue is drained later by send_task when idle slots exist;
 * the bitmap is used for the dispatched-skip check and the near-ready lookahead. */
static inline void mark_near_ready_queue(void)
{
    for (int type = 0; type < TASK_TYPE_CNT; type++) {
        queue_t *q = &g_near_ready_queue[type];
        uint64_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        uint64_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        uint64_t avail = tail - head;
        const uint32_t mask = RQ_SIZE - 1;

        for (uint64_t i = 0; i < avail; i++) {
            uint32_t task_id = q->tasks[(head + i) & mask];
            task_state_set(task_id, TASK_STATE_NEAR_READY);
        }
    }
}

static inline bool has_idle_slot(ctrl_t *ctrl)
{
    for (int type = 0; type < TASK_TYPE_CNT; type++) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            if (ctrl->free_bitmap[type][slot] & ctrl->aicore_mask) {
                return true;
            }
        }
    }
    return false;
}

/* Find which subgraph owns `task_id`. The per-painter task_id arrays partition
 * the global task space, so this returns the subgraph whose successors[] should
 * be used for dependency lookahead. */
static inline int task_subgraph(uint32_t task_id)
{
    for (int p = 0; p < PAINTER_THREAD_CNT; p++) {
        uint32_t cnt = test_graph[p].task_cnt;
        const uint32_t *ids = test_graph[p].task_id;
        for (uint32_t i = 0; i < cnt; i++) {
            if (ids[i] == task_id) {
                return p;
            }
        }
    }
    return 0;
}

/* Dispatch a single near-ready task to an idle slot of its own type.
 * The state transition NEAR_READY -> DISPATCHED is done atomically first, so
 * with multiple dispatch threads each task is claimed by exactly one thread. */
static inline bool dispatch_one_task(ctrl_t *ctrl, uint32_t task_id)
{
    int type = total_type[task_id];

    for (int slot = 0; slot < AIC_OSTD; slot++) {
        uint64_t free_bits = ctrl->free_bitmap[type][slot] & ctrl->aicore_mask;
        if (free_bits == 0) {
            continue;
        }

        uint64_t idx = (uint64_t)__builtin_ctzll(free_bits);
        uint64_t mask = (uint64_t)0x1 << idx;

        if (!task_try_dispatch(task_id)) {
            return false;
        }

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

        ctrl->free_bitmap[type][slot] &= ~mask;
        #ifndef REAL_CHIP
        ctrl->msg_bitmap[type][slot] |= mask;
        #endif

        WORKER_LOGF("send_near_ready,task_id,%u,core,%llu,slot,%d,type,%d",
                    task_id, (unsigned long long)idx, slot, type);
        return true;
    }

    return false;
}

/* Opportunistically fill remaining idle slots by walking every DISPATCHED
 * task and dispatching its NEAR_READY successors. This chains a successor onto
 * its last running predecessor instead of waiting for completion to reach the
 * painter. */
static inline int dispatch_near_ready_successors(ctrl_t *ctrl)
{
    int sent = 0;

    if (!has_idle_slot(ctrl)) {
        return sent;
    }

    for (uint32_t task_id = 0; task_id < total_task_cnt; task_id++) {
        if (!task_is_dispatched(task_id)) {
            continue;
        }

        int sub = task_subgraph(task_id);
        int suc_cnt = test_graph[sub].suc_cnt[task_id];
        if (suc_cnt <= 0) {
            continue;
        }

        int idx = test_graph[sub].suc_idx[task_id];
        for (int k = idx; k < idx + suc_cnt; k++) {
            uint32_t succ_id = (uint32_t)test_graph[sub].successors[k];
            if (!task_is_near_ready(succ_id)) {
                continue;
            }

            if (dispatch_one_task(ctrl, succ_id)) {
                sent++;
                if (!has_idle_slot(ctrl)) {
                    return sent;
                }
            }
        }
    }

    return sent;
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

            /* A task already handed to a slot (e.g. a near-ready successor
             * dispatched by the lookahead path) must not be sent again. */
            if (task_is_dispatched(task_id) || task_is_completed(task_id)) {
                continue;
            }

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

            task_mark_dispatched(task_id);

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

    /* Mark tasks sitting in the near-ready queue so the lookahead pass below
     * can chain them onto their last running predecessor. */
    mark_near_ready_queue();

    read_msgq(tid);
    push_2_completed_queue(tid);

    total_sent += send_task(&g_ctrl_t[tid], &g_ready_queue[TASK_TYPE_VECTOR], TASK_TYPE_VECTOR, &has_idle_slot);
    total_sent += send_task(&g_ctrl_t[tid], &g_ready_queue[TASK_TYPE_CUBE], TASK_TYPE_CUBE, &has_idle_slot);

    /* Drain near-ready tasks when a slot is still idle. This keeps near-ready
     * tasks flowing even after their last predecessor has been collected as
     * completed (the lookahead pass below only chains off DISPATCHED tasks). */
    if (has_idle_slot) {
        if (!queue_empty(&g_near_ready_queue[TASK_TYPE_VECTOR])) {
            total_sent += send_task(&g_ctrl_t[tid], &g_near_ready_queue[TASK_TYPE_VECTOR],
                                    TASK_TYPE_VECTOR, NULL);
        }
        if (!queue_empty(&g_near_ready_queue[TASK_TYPE_CUBE])) {
            total_sent += send_task(&g_ctrl_t[tid], &g_near_ready_queue[TASK_TYPE_CUBE],
                                    TASK_TYPE_CUBE, NULL);
        }
    }

    /* Fill any remaining idle slots with near-ready successors of dispatched
     * tasks, and mark those successors as dispatched. */
    total_sent += dispatch_near_ready_successors(&g_ctrl_t[tid]);

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
