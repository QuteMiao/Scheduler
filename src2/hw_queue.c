#include "hw_queue.h"
#include "cases2/qwen3_14b_decode.h"
/* ---------------------------------------------------------------------
 * three-level queue instances, mirroring the hardware TOPO bottom-up:
 *   12 cluster queues + 2 die queues + 1 chip queue
 * --------------------------------------------------------------------- */
task_queue_desc_t g_chip_queue[TASK_TYPE_CNT];
task_queue_desc_t g_die_queue[DIE_QUEUE_NUM][TASK_TYPE_CNT];
cluster_queue_t g_cluster_queue[CLUSTER_QUEUE_NUM][TASK_TYPE_CNT];

task_queue_desc_t g_die_complete_queue[DIE_COMPLETE_QUEUE_NUM];
cluster_queue_t g_cluster_complete_queue[CLUSTER_COMPLETE_QUEUE_NUM];

/* ---------------------------------------------------------------------
 * CTR (Cluster Tensor Register) backing. The CTR registers are NOT at
 * contiguous addresses: each AICore owns one CUBE and one VECTOR unit and
 * each unit exposes 8 consecutive 64-bit registers. A cluster queue spans
 * several AICores, so each queue stores an array of per-slot hardware
 * register addresses that queue_init() fills from a6.h.
 * --------------------------------------------------------------------- */
#define CLUSTER_QUEUE_MAX_DEPTH CLUSTER_MIX_QUEUE_DEPTH
static uint64_t g_cluster_queue_addrs[CLUSTER_QUEUE_NUM][TASK_TYPE_CNT][CLUSTER_QUEUE_MAX_DEPTH];
static uint64_t g_cluster_complete_queue_addrs[CLUSTER_COMPLETE_QUEUE_NUM][CLUSTER_COMPLETE_QUEUE_DEPTH];

/* which register file(s) back a cluster queue */
typedef enum {
    UNIT_CUBE   = 0,
    UNIT_VECTOR = 1,
    UNIT_BOTH   = 2,
} cluster_unit_t;

static uint64_t aicore_unit_reg_addr(uint8_t die_id, uint8_t aicore_id,
                                     uint8_t unit, uint8_t reg_idx)
{
    uint64_t base = (unit == UNIT_CUBE)
        ? aicore_cube_reg_base(die_id, aicore_id)
        : aicore_vector_reg_base(die_id, aicore_id);
    return base + (uint64_t)reg_idx * CTR_REG_BYTES;
}

/* fill `addrs[0..capacity-1]` with the hardware register addresses of one
 * cluster queue. `units` selects CUBE-only / VECTOR-only / both interleaved;
 * `reg_offset` + `regs_per_unit` select the registers inside each unit. */
static void fill_cluster_queue_addrs(uint8_t die_id, uint8_t cluster_id,
                                     uint64_t *addrs, uint32_t capacity,
                                     cluster_unit_t units,
                                     uint8_t reg_offset, uint8_t regs_per_unit)
{
    uint8_t  num_units       = (units == UNIT_BOTH) ? 2 : 1;
    uint8_t  first_unit      = (units == UNIT_VECTOR) ? UNIT_VECTOR : UNIT_CUBE;
    uint32_t regs_per_aicore = (uint32_t)regs_per_unit * (uint32_t)num_units;

    for (uint32_t i = 0; i < capacity; i++) {
        uint32_t k         = i / regs_per_aicore;   /* AICore index in cluster */
        uint32_t loc       = i % regs_per_aicore;
        uint8_t  unit      = (uint8_t)(first_unit + loc / regs_per_unit);
        uint8_t  reg       = (uint8_t)(reg_offset + loc % regs_per_unit);
        uint8_t  aicore_id = (uint8_t)(cluster_id * AICORE_PER_CLUSTER + k);

        addrs[i] = aicore_unit_reg_addr(die_id, aicore_id, unit, reg);
    }
}

static void init_cluster_queue(cluster_queue_t *q, uint64_t *addrs, uint32_t capacity,
                               uint8_t die_id, uint8_t cluster_id,
                               cluster_unit_t units,
                               uint8_t reg_offset, uint8_t regs_per_unit)
{
    fill_cluster_queue_addrs(die_id, cluster_id, addrs, capacity,
                             units, reg_offset, regs_per_unit);
    q->reg_addrs = addrs;
    q->capacity  = capacity;
    q->head      = 0;
    q->tail      = 0;
    atomic_flag_clear(&q->head_lock);
    atomic_flag_clear(&q->tail_lock);
}

/* build the three-level queues bottom-up, TASK_TYPE_CNT queues per level */
void queue_init(void)
{
    /* level 0: cluster queues, 6 * 2 = 12 clusters x 3 types, CTR registers.
     * Each queue is backed by a table of non-contiguous hardware register
     * addresses filled from a6.h. */
    for (uint8_t die = 0; die < DIE_NUM; die++) {
        for (uint8_t cluster = 0; cluster < CLUSTER_PER_DIE; cluster++) {
            uint8_t c = die * CLUSTER_PER_DIE + cluster;

            /* CUBE queue: CUBE unit reg[0..1] of every AICore */
            init_cluster_queue(&g_cluster_queue[c][CUBE],
                               g_cluster_queue_addrs[c][CUBE],
                               CLUSTER_QUEUE_DEPTH, die, cluster, UNIT_CUBE,
                               CLUSTER_QUEUE_REG_OFFSET, CLUSTER_QUEUE_REGS_PER_UNIT);

            /* VECTOR queue: VECTOR unit reg[0..1] of every AICore */
            init_cluster_queue(&g_cluster_queue[c][VECTOR],
                               g_cluster_queue_addrs[c][VECTOR],
                               CLUSTER_QUEUE_DEPTH, die, cluster, UNIT_VECTOR,
                               CLUSTER_QUEUE_REG_OFFSET, CLUSTER_QUEUE_REGS_PER_UNIT);

            /* MIX queue: CUBE unit reg[6..7] of every AICore */
            init_cluster_queue(&g_cluster_queue[c][MIX],
                               g_cluster_queue_addrs[c][MIX],
                               CLUSTER_MIX_QUEUE_DEPTH, die, cluster, UNIT_CUBE,
                               MIX_QUEUE_REG_OFFSET, MIX_QUEUE_REGS_PER_UNIT);

            /* complete queue: reg[2..5] of both CUBE and VECTOR units */
            init_cluster_queue(&g_cluster_complete_queue[c],
                               g_cluster_complete_queue_addrs[c],
                               CLUSTER_COMPLETE_QUEUE_DEPTH, die, cluster, UNIT_BOTH,
                               COMPLETE_QUEUE_REG_OFFSET, COMPLETE_QUEUE_REGS_PER_UNIT);
        }
    }

    /* level 1: die queues, 2 dies x 3 types */
    for (uint8_t die = 0; die < DIE_NUM; die++) {
        for (uint8_t t = 0; t < TASK_TYPE_CNT; t++) {
            g_die_queue[die][t].base = die_queue_base(die, (task_type_t)t);
        }
    }

    /* level 2: chip queue, 1 chip x 3 types */
    for (uint8_t t = 0; t < TASK_TYPE_CNT; t++) {
        g_chip_queue[t].base = chip_queue_base((task_type_t)t);
    }

    /* complete queues: 2 die (GQM), no type split */
    for (uint8_t die = 0; die < DIE_NUM; die++) {
        g_die_complete_queue[die].base = die_complete_queue_base(die);
    }
}

/* ---------------------------------------------------------------------
 * CTR cluster queue enqueue / dequeue (ring buffer + head/tail locks).
 * --------------------------------------------------------------------- */

static inline void cq_lock(atomic_flag *lock)
{
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {
        /* spin */
    }
}

static inline void cq_unlock(atomic_flag *lock)
{
    atomic_flag_clear_explicit(lock, memory_order_release);
}

bool cluster_queue_push(cluster_queue_t *q, uint64_t task)
{
    cq_lock(&q->tail_lock);

    uint32_t next = (q->tail + 1) % q->capacity;
    if (next == q->head) {          /* full: one slot reserved */
        cq_unlock(&q->tail_lock);
        return false;
    }

    ctr_reg_write(q->reg_addrs[q->tail], task);
    q->tail = next;

    cq_unlock(&q->tail_lock);
    return true;
}

bool cluster_queue_pop(cluster_queue_t *q, uint64_t *task)
{
    cq_lock(&q->head_lock);

    if (q->head == q->tail) {       /* empty */
        cq_unlock(&q->head_lock);
        return false;
    }

    *task = ctr_reg_read(q->reg_addrs[q->head]);
    q->head = (q->head + 1) % q->capacity;

    cq_unlock(&q->head_lock);
    return true;
}

/* ---------------------------------------------------------------------
 * PUSH / POP by continuous core_id and task type.
 * the owning die/cluster is resolved from core_id; within each level the
 * queue of the requested task type is used. the task is first routed to the
 * nearest cluster queue, and on failure it falls back to the owning die
 * queue and finally the chip queue (bottom-up levels).
 * --------------------------------------------------------------------- */

bool queue_push(uint32_t core_id, task_type_t type, uint64_t task)
{
    uint8_t die_id     = core_id_to_die(core_id);
    uint8_t cluster_id = core_id_to_cluster(core_id);
    uint32_t task_id   = (uint32_t)task;
    uint8_t cluster_q  = die_id * CLUSTER_PER_DIE + cluster_id;

    /* level 0: nearest cluster queue of the given type */
    if (cluster_queue_push(&g_cluster_queue[cluster_q][type], task)) {
        task_coord[task_id] = TASK_COORD(die_id, cluster_id);
        return true;
    }

    /* level 1: owning die queue of the given type (cluster full -> cluster unknown) */
    if (gqm_push(g_die_queue[die_id][type].base, task)) {
        task_coord[task_id] = TASK_COORD(die_id, TASK_COORD_INVALID);
        return true;
    }

    /* level 2: chip queue of the given type (die full -> die/cluster unknown) */
    if (gqm_push(g_chip_queue[type].base, task)) {
        task_coord[task_id] = TASK_COORD(TASK_COORD_INVALID, TASK_COORD_INVALID);
        return true;
    }

    return false;
}

bool queue_pop(uint32_t core_id, task_type_t type, uint64_t *task)
{
    uint8_t die_id     = core_id_to_die(core_id);
    uint8_t cluster_id = core_id_to_cluster(core_id);
    uint8_t cluster_q  = die_id * CLUSTER_PER_DIE + cluster_id;

    /* level 0: nearest cluster queue of the given type */
    if (cluster_queue_pop(&g_cluster_queue[cluster_q][type], task)) {
        task_coord[(uint32_t)*task] = TASK_COORD(die_id, cluster_id);
        return true;
    }

    /* level 1: owning die queue of the given type */
    if (gqm_pop(g_die_queue[die_id][type].base, task)) {
        task_coord[(uint32_t)*task] = TASK_COORD(die_id, cluster_id);
        return true;
    }

    /* level 2: chip queue of the given type */
    if (gqm_pop(g_chip_queue[type].base, task)) {
        task_coord[(uint32_t)*task] = TASK_COORD(die_id, cluster_id);
        return true;
    }

    return false;
}

/* ---------------------------------------------------------------------
 * PUSH to the queue nearest a predecessor's coordinate.
 *
 * Mirrors queue_push(), but instead of resolving the target from a continuous
 * core_id it starts from the packed (die_id, cluster_id) coordinate of a
 * task's predecessor (as recorded in task_coord[]), so a successor is
 * scheduled onto the cluster that just produced its data (best locality).
 * On failure it falls back to the owning die queue and finally the chip
 * queue, exactly like queue_push().
 * --------------------------------------------------------------------- */
bool queue_push_to_pre_coord(int coord, task_type_t type, uint64_t task)
{
    uint8_t die_id     = (uint8_t)(coord >> 8);
    uint8_t cluster_id = (uint8_t)(coord & 0xFF);
    uint32_t task_id   = (uint32_t)task;

    /* level 0: cluster queue nearest the predecessor (skip when the
     * predecessor's die/cluster is unknown, i.e. it was demoted upward) */
    if (die_id < DIE_NUM && cluster_id < CLUSTER_PER_DIE) {
        uint8_t cluster_q = die_id * CLUSTER_PER_DIE + cluster_id;
        if (cluster_queue_push(&g_cluster_queue[cluster_q][type], task)) {
            task_coord[task_id] = TASK_COORD(die_id, cluster_id);
            return true;
        }
    }

    /* level 1: owning die queue (cluster full/unknown -> cluster unknown) */
    if (die_id < DIE_NUM) {
        if (gqm_push(g_die_queue[die_id][type].base, task)) {
            task_coord[task_id] = TASK_COORD(die_id, TASK_COORD_INVALID);
            return true;
        }
    }

    /* level 2: chip queue (die full/unknown -> die/cluster both unknown) */
    if (gqm_push(g_chip_queue[type].base, task)) {
        task_coord[task_id] = TASK_COORD(TASK_COORD_INVALID, TASK_COORD_INVALID);
        return true;
    }

    return false;
}

/* ---------------------------------------------------------------------
 * PUSH by global cluster_id (0..CLUSTER_NUM-1) and task type.
 *
 * Unlike queue_push() (which resolves the cluster from a core_id), this pushes
 * directly to the cluster queue identified by cluster_id. On failure it falls
 * back to the owning die queue.
 * --------------------------------------------------------------------- */
bool queue_push_by_cluster(uint32_t cluster_id, task_type_t type, uint64_t task)
{
    uint8_t die_id   = (uint8_t)(cluster_id / CLUSTER_PER_DIE);
    uint32_t task_id = (uint32_t)task;

    /* level 0: the target cluster queue of the given type */
    if (cluster_queue_push(&g_cluster_queue[cluster_id][type], task)) {
        task_coord[task_id] = TASK_COORD(die_id, cluster_id);
        return true;
    }

    /* level 1: owning die queue (cluster full -> cluster unknown) */
    if (gqm_push(g_die_queue[die_id][type].base, task)) {
        task_coord[task_id] = TASK_COORD(die_id, TASK_COORD_INVALID);
        return true;
    }

    return false;
}

/* ---------------------------------------------------------------------
 * Complete queue PUSH / POP by continuous core_id (no task_type).
 *
 * When a task finishes, its id is recorded in a "complete" queue so that
 * dependents / observers can learn about the completion. The nearest cluster
 * complete queue is tried first, falling back to the owning die complete
 * queue. Complete queues are NOT split by task type.
 * --------------------------------------------------------------------- */

bool complete_queue_push(uint32_t core_id, uint64_t task)
{
    uint8_t die_id     = core_id_to_die(core_id);
    uint8_t cluster_id = core_id_to_cluster(core_id);
    uint8_t cluster_q  = die_id * CLUSTER_PER_DIE + cluster_id;

    /* level 0: nearest cluster complete queue */
    if (cluster_queue_push(&g_cluster_complete_queue[cluster_q], task)) {
        return true;
    }

    /* level 1: owning die complete queue */
    if (gqm_push(g_die_complete_queue[die_id].base, task)) {
        return true;
    }

    return false;
}

bool complete_queue_pop(uint32_t core_id, uint64_t *task)
{
    uint8_t die_id     = core_id_to_die(core_id);
    uint8_t cluster_id = core_id_to_cluster(core_id);
    uint8_t cluster_q  = die_id * CLUSTER_PER_DIE + cluster_id;

    /* level 0: nearest cluster complete queue */
    if (cluster_queue_pop(&g_cluster_complete_queue[cluster_q], task)) {
        return true;
    }

    /* level 1: owning die complete queue */
    if (gqm_pop(g_die_complete_queue[die_id].base, task)) {
        return true;
    }

    return false;
}
