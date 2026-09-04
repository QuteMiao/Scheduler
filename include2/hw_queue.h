#ifndef __HW_QUEUE_H__
#define __HW_QUEUE_H__

#include <stdint.h>
#include <stdbool.h>

#define CLUSTER_PER_DIE 6
#define AICORE_PER_CLUSTER 10
#define CUBE_PER_AICORE 1
#define VECTOR_PER_AICORE 1

/* =====================================================================
 * hardware topology (from a6.h):
 *
 *   CHIP (1) -> DIE (2) -> CLUSTER (6 per die) -> AICORE (10 per cluster)
 *
 * ===================================================================== */
#define CHIP_NUM           1
#define DIE_NUM            2                     /* 2 */
#define CLUSTER_NUM        (DIE_NUM * CLUSTER_PER_DIE) /* 12 */

/* =====================================================================
 * three-level queue, built bottom-up to mirror the hardware TOPO:
 *
 *   CLUSTER_QUEUE : 6 * 2 = 12
 *   DIE_QUEUE     : 2
 *   CHIP_QUEUE    : 1
 *
 * every level is further split into TASK_TYPE_CNT dedicated queues, one per
 * task type (CUBE / VECTOR / MIX), for a total of:
 *
 *   CLUSTER_QUEUE : 12 * 3 = 36
 *   DIE_QUEUE     :  2 * 3 =  6
 *   CHIP_QUEUE    :  1 * 3 =  3
 *
 * ===================================================================== */
#define CLUSTER_QUEUE_NUM  (CLUSTER_PER_DIE * DIE_NUM) /* 6 * 2 = 12 */
#define DIE_QUEUE_NUM      DIE_NUM                     /* 2 */
#define CHIP_QUEUE_NUM     1                       /* 1 */

/* number of task types -> a dedicated queue per type at every level */
#define TASK_TYPE_CNT      3                           /* CUBE / VECTOR / MIX */

/* complete queues: store finished task_ids. Unlike the dispatch queues above,
 * they are NOT split by task_type — one complete queue per cluster and per die. */
#define CLUSTER_COMPLETE_QUEUE_NUM CLUSTER_QUEUE_NUM   /* 12 */
#define DIE_COMPLETE_QUEUE_NUM     DIE_QUEUE_NUM       /* 2 */

#define TCM_BASE 0x1
#define TCM_DIE_OFFSET 0x1
#define TCM_CLUSTER_OFFSET 0x1
#define TCM_TYPE_OFFSET 0x1
#define TCM_COMPLETE_OFFSET 0x1

#define CHIP_QUEUE_SIZE 0x1
#define DIE_QUEUE_SIZE 0x1

/* continuous core layout: cores are numbered 0..TOTAL_CORE_NUM-1 across the
 * whole chip; from a core_id the owning die and cluster can be resolved */
#define CORE_PER_DIE   (CLUSTER_PER_DIE * AICORE_PER_CLUSTER) /* 6 * 10 = 60 */
#define TOTAL_CORE_NUM (DIE_NUM * CORE_PER_DIE)               /* 2 * 60 = 120 */

/* task types: each level keeps TASK_TYPE_CNT dedicated queues, one per type */
typedef enum {
    CUBE   = 0,
    VECTOR = 1,
    MIX    = 2,
    CNT    = TASK_TYPE_CNT,
} task_type_t;

/* MSGQ register base address of each queue level, offset by task_type so that
 * the CUBE / VECTOR / MIX queues of a level sit at consecutive addresses */
static inline uint64_t chip_queue_base(task_type_t type)
{
    return (uint64_t)TCM_BASE + (uint64_t)TCM_TYPE_OFFSET * (uint64_t)type;
}

static inline uint64_t die_queue_base(uint8_t die_id, task_type_t type)
{
    return (uint64_t)TCM_BASE + CHIP_QUEUE_SIZE
         + (uint64_t)TCM_DIE_OFFSET * (uint64_t)die_id
         + (uint64_t)TCM_TYPE_OFFSET * (uint64_t)type;
}

static inline uint64_t cluster_queue_base(uint8_t die_id, uint8_t cluster_id, task_type_t type)
{
    return (uint64_t)TCM_BASE
         + (uint64_t)TCM_DIE_OFFSET * (uint64_t)die_id
         + (uint64_t)TCM_CLUSTER_OFFSET * (uint64_t)cluster_id
         + (uint64_t)TCM_TYPE_OFFSET * (uint64_t)type;
}

/* complete queue base addresses: no task_type dimension */
static inline uint64_t die_complete_queue_base(uint8_t die_id)
{
    return (uint64_t)TCM_BASE
         + (uint64_t)TCM_COMPLETE_OFFSET
         + (uint64_t)TCM_DIE_OFFSET * (uint64_t)die_id;
}

static inline uint64_t cluster_complete_queue_base(uint8_t die_id, uint8_t cluster_id)
{
    return (uint64_t)TCM_BASE
         + (uint64_t)TCM_COMPLETE_OFFSET
         + (uint64_t)TCM_DIE_OFFSET * (uint64_t)die_id
         + (uint64_t)TCM_CLUSTER_OFFSET * (uint64_t)cluster_id;
}

/* resolve the owning die / cluster from a continuous core_id */
static inline uint8_t core_id_to_die(uint32_t core_id)
{
    return (uint8_t)(core_id / CORE_PER_DIE);
}

static inline uint8_t core_id_to_cluster(uint32_t core_id)
{
    return (uint8_t)((core_id / AICORE_PER_CLUSTER) % CLUSTER_PER_DIE);
}

typedef struct {
    uint64_t base;       /* hardware register base address */
    uint16_t  size;      /* QUEUE_LEVEL_* */
} task_queue_desc_t;

/* the three-level queue instances (bottom-up), each split into TASK_TYPE_CNT
 * dedicated queues keyed by task_type_t */
extern task_queue_desc_t g_chip_queue[TASK_TYPE_CNT];
extern task_queue_desc_t g_die_queue[DIE_QUEUE_NUM][TASK_TYPE_CNT];
extern task_queue_desc_t g_cluster_queue[CLUSTER_QUEUE_NUM][TASK_TYPE_CNT];

/* complete queue instances (finished task_ids, no task_type split) */
extern task_queue_desc_t g_die_complete_queue[DIE_COMPLETE_QUEUE_NUM];
extern task_queue_desc_t g_cluster_complete_queue[CLUSTER_COMPLETE_QUEUE_NUM];

/* build all queues bottom-up */
void queue_init(void);

/* GQM hardware queue primitive: PUSH/POP via hardware instructions.
 * the first parameter is the queue's hardware register base address */
bool gqm_push(uint64_t queue_base, uint64_t task);
bool gqm_pop(uint64_t queue_base, uint64_t *task);

/* total_task_coord[]: task_id -> the (die_id, cluster_id) coordinate of the
 * queue it was last pushed to / popped from. The array is defined once by the
 * workload case header (under cases2/), sized to total_task_cnt; queue_push() and
 * queue_pop() keep it up to date. */
extern int task_coord[];

/* Pack die_id / cluster_id into the single int stored in total_task_coord[]:
 *   high byte = die_id, low byte = cluster_id */
#define TASK_COORD(die_id, cluster_id) (((int)(die_id) << 8) | (int)(cluster_id))

/* Sentinel for die_id / cluster_id written to total_task_coord[] when a task
 * is placed in a higher-level queue (die/chip) because its owning cluster/die
 * queue was full. 0xFF is outside the valid ranges (die_id <= 1, cluster_id <= 5). */
#define TASK_COORD_INVALID 0xFF

/* PUSH/POP by continuous core_id and task_type; the nearest cluster/die is
 * resolved automatically from core_id, the queue of the given type is chosen,
 * and on failure it falls back to the upper-level queue (cluster -> die -> chip) */
bool queue_push(uint32_t core_id, task_type_t type, uint64_t task);
bool queue_pop(uint32_t core_id, task_type_t type, uint64_t *task);

/* PUSH to the queue nearest a predecessor's packed (die_id, cluster_id)
 * coordinate, so a successor is scheduled onto the cluster that produced its
 * data. coord is a TASK_COORD() value; the cluster -> die -> chip fallback
 * mirrors queue_push(). */
bool queue_push_to_pre_coord(int coord, task_type_t type, uint64_t task);

/* PUSH by global cluster_id (0..CLUSTER_NUM-1) and task_type. The task is
 * pushed to that cluster's queue of the given type; on failure it falls back
 * to the owning die queue. */
bool queue_push_by_cluster(uint32_t cluster_id, task_type_t type, uint64_t task);

/* Complete queue PUSH/POP by continuous core_id (no task_type). A finished
 * task's id is pushed to the nearest cluster complete queue, falling back to
 * the owning die complete queue. */
bool queue_push_complete(uint32_t core_id, uint64_t task);
bool queue_pop_complete(uint32_t core_id, uint64_t *task);

#endif /* __HW_QUEUE_H__ */
