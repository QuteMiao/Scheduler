#ifndef COMMON_QUEUE_H
#define COMMON_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#include "conf.h"

/*
 * Multi-producer / multi-consumer ready queue (bounded ring buffer).
 *
 * The original single spinlock guarding both ends is split into two
 * independent locks so producers and consumers no longer block each other:
 *
 *   - tail + tail_lock are owned by producers (painters -> batch_enqueue).
 *     Multiple producers serialize on tail_lock but run concurrently with
 *     consumers.
 *   - head + head_lock are owned by consumers (dispatchers -> batch_dequeue).
 *     Multiple consumers serialize on head_lock.
 *
 * head/tail are monotonic 64-bit counters; the ring index is always
 * counter & (RQ_SIZE - 1), and the number of buffered items is tail - head, so
 * the old cnt field is gone.
 *
 * Ordering: producers write items, then publish tail with release; consumers
 * load tail with acquire before reading items.  Each side reads the other's
 * counter only for the full/empty fast path, and a stale read is always
 * conservative (reports the queue as more full / more empty), never causing
 * unread data to be overwritten or garbage to be returned.
 *
 * The four hot fields are cache-line aligned so producer and consumer state
 * never share a cache line (avoids false sharing).
 */
typedef struct queue {
    /* consumer-owned */
    _Alignas(CACHE_LINE_SIZE) _Atomic uint64_t head;
    _Alignas(CACHE_LINE_SIZE) atomic_flag head_lock;
    /* producer-owned */
    _Alignas(CACHE_LINE_SIZE) _Atomic uint64_t tail;
    _Alignas(CACHE_LINE_SIZE) atomic_flag tail_lock;
    uint32_t tasks[RQ_SIZE];
} queue_t;

static inline void spin_lock(atomic_flag *lock);
static inline void spin_unlock(atomic_flag *lock);

static inline bool batch_dequeue(queue_t *queue, uint32_t *item, uint32_t *n)
{
    spin_lock(&queue->head_lock);

    uint64_t tail = atomic_load_explicit(&queue->tail, memory_order_acquire);
    uint64_t head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    uint64_t avail = tail - head;
    *n = (uint32_t)(avail < *n ? avail : *n);
    if (*n == 0) {
        spin_unlock(&queue->head_lock);
        return false;
    }

    const uint32_t mask = RQ_SIZE - 1;
    uint32_t h = (uint32_t)(head & mask);
    uint32_t contiguous = RQ_SIZE - h;
    if (*n <= contiguous) {
        memcpy(item, &queue->tasks[h], *n * sizeof(uint32_t));
    } else {
        memcpy(item, &queue->tasks[h], contiguous * sizeof(uint32_t));
        memcpy(item + contiguous, &queue->tasks[0], (*n - contiguous) * sizeof(uint32_t));
    }

    atomic_store_explicit(&queue->head, head + *n, memory_order_release);
    spin_unlock(&queue->head_lock);
    return true;
}

static inline bool batch_enqueue(queue_t *queue, uint32_t *item, uint32_t n)
{
    if (n == 0) {
        return true;
    }

    spin_lock(&queue->tail_lock);

    uint64_t head = atomic_load_explicit(&queue->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
    if ((tail - head) + n > RQ_SIZE) {
        spin_unlock(&queue->tail_lock);
        return false;
    }

    const uint32_t mask = RQ_SIZE - 1;
    uint32_t t = (uint32_t)(tail & mask);
    uint32_t contiguous = RQ_SIZE - t;
    if (n <= contiguous) {
        memcpy(&queue->tasks[t], item, n * sizeof(uint32_t));
    } else {
        memcpy(&queue->tasks[t], item, contiguous * sizeof(uint32_t));
        memcpy(&queue->tasks[0], item + contiguous, (n - contiguous) * sizeof(uint32_t));
    }

    atomic_store_explicit(&queue->tail, tail + n, memory_order_release);
    spin_unlock(&queue->tail_lock);
    return true;
}

/* Single-item convenience wrappers: same two-lock scheme with batch size 1. */
static inline bool dequeue(queue_t *queue, uint32_t *item)
{
    uint32_t n = 1;
    return batch_dequeue(queue, item, &n);
}

static inline bool enqueue(queue_t *queue, uint32_t item)
{
    return batch_enqueue(queue, &item, 1);
}

static inline void spin_lock(atomic_flag *lock)
{
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {
        atomic_thread_fence(memory_order_seq_cst);
    }
}

static inline void spin_unlock(atomic_flag *lock)
{
    atomic_flag_clear_explicit(lock, memory_order_release);
}

#endif /* COMMON_QUEUE_H */
