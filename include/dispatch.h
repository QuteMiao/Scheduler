/*
 * dispatch.h - Task Dispatch with Shared Memory and Work-Stealing
 *
 * Distributes tasks to Executors via shared memory with work-stealing
 * load balancing across multiple Dispatch instances.
 *
 * Trust the Caller (Principle X): No input validation, undefined on invalid input.
 * C11 standard with _Atomic for lock-free concurrency.
 */

#ifndef SCHEDULER_DISPATCH_H
#define SCHEDULER_DISPATCH_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#include "conf.h"
#include "queue.h"
#include "task.h"

/*
 * Multi-producer / multi-reader completed queue.
 *
 * All dispatch threads push completed task ids here (writers are serialized
 * by a spinlock), and every painter thread drains the same ring independently
 * through its own read cursor. An entry becomes overwritable only once ALL
 * painters have advanced past it (write_pos - min(read_pos[]) <= CQ_SIZE).
 */
typedef struct {
    uint32_t ring[CQ_SIZE];
    cacheline_u64_t write_pos;                    /* written by dispatchers, read by all painters */
    cacheline_u64_t read_pos[PAINTER_THREAD_CNT]; /* one private cache line per painter */
    cacheline_flag_t write_lock;                  /* serializes writers */
} completed_queue_t;

typedef struct ctrl {
    // 64CORES
    uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];
    
    uint64_t aicore_mask;

    uint32_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint32_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];

    uint64_t* aicore_spr_1[EXE_TYPE_CNT][AIC_CNT];
    uint64_t* aicore_spr_2[EXE_TYPE_CNT][AIC_CNT];

    uint32_t tid;
} ctrl_t;

/* Global, thread-shared queues (single instance for the whole scheduler). */
extern queue_t g_ready_queue[TASK_TYPE_CNT];
extern queue_t g_near_ready_queue[TASK_TYPE_CNT];
extern completed_queue_t g_completed_queue;

void *dispatch_worker(void *arg);
void init_ctrl_t(void);

/* Batch-write completed tasks to the shared multi-reader ring buffer.
 * Multi-producer safe: writers serialize on write_lock. */
static inline void completed_queue_write_batch(completed_queue_t *q, uint32_t *items, uint32_t cnt)
{
    if (cnt == 0) return;
    while (atomic_flag_test_and_set_explicit(&q->write_lock.v, memory_order_acquire)) {
        atomic_thread_fence(memory_order_seq_cst);
    }
    uint64_t wpos = atomic_load_explicit(&q->write_pos.v, memory_order_relaxed);
    uint32_t *ring = q->ring;
    for (uint32_t i = 0; i < cnt; i++) {
        ring[(wpos + i) & CQ_MASK] = items[i];
    }
    atomic_store_explicit(&q->write_pos.v, wpos + cnt, memory_order_release);
    atomic_flag_clear_explicit(&q->write_lock.v, memory_order_release);
}

/* Zero-copy batch-read from the shared multi-reader ring buffer for a specific
 * painter. Returns a pointer to up to max_cnt contiguous items sitting at this
 * painter's read cursor (no copy), and stores the actual count in *cnt.
 *
 * The read cursor is NOT advanced here - call completed_queue_read_commit()
 * once the returned items have been consumed. The batch is clamped so it never
 * wraps past the end of the ring, keeping the returned range contiguous.
 * Returns NULL when nothing new is available. */
static inline const uint32_t *completed_queue_read_batch(completed_queue_t *q, int painter_tid,
                                                         uint32_t *cnt, uint32_t max_cnt)
{
    _Atomic uint64_t *my_read = &q->read_pos[painter_tid].v;
    uint64_t rpos = atomic_load_explicit(my_read, memory_order_acquire);
    uint64_t wpos = atomic_load_explicit(&q->write_pos.v, memory_order_acquire);

    uint64_t avail = wpos - rpos;
    if (avail <= 0) {
        *cnt = 0;
        return NULL;
    }

    uint32_t n = (uint32_t)(avail < max_cnt ? avail : max_cnt);

    /* Clamp to the physical end of the ring so the returned range is contiguous. */
    uint32_t span = CQ_SIZE - (uint32_t)(rpos & CQ_MASK);
    if (n > span) n = span;

    *cnt = n;
    return &q->ring[rpos & CQ_MASK];
}

/* Advance a painter's read cursor after a zero-copy batch has been consumed. */
static inline void completed_queue_read_commit(completed_queue_t *q, int painter_tid, uint32_t cnt)
{
    if (cnt == 0) return;
    atomic_fetch_add_explicit(&q->read_pos[painter_tid].v, (uint64_t)cnt, memory_order_release);
}

#endif /* SCHEDULER_DISPATCH_H */