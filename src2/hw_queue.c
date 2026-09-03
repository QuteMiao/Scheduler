#include "hw_queue.h"

/* ---------------------------------------------------------------------
 * three-level queue instances, mirroring the hardware TOPO bottom-up:
 *   12 cluster queues + 2 die queues + 1 chip queue
 * --------------------------------------------------------------------- */
task_queue_desc_t g_chip_queue[TASK_TYPE_CNT];
task_queue_desc_t g_die_queue[DIE_QUEUE_NUM][TASK_TYPE_CNT];
task_queue_desc_t g_cluster_queue[CLUSTER_QUEUE_NUM][TASK_TYPE_CNT];

/* build the three-level queues bottom-up, TASK_TYPE_CNT queues per level */
void queue_init(void)
{
    /* level 0: cluster queues, 6 * 2 = 12 clusters x 3 types */
    for (uint8_t die = 0; die < DIE_NUM; die++) {
        for (uint8_t cluster = 0; cluster < CLUSTER_PER_DIE; cluster++) {
            for (uint8_t t = 0; t < TASK_TYPE_CNT; t++) {
                g_cluster_queue[die * CLUSTER_PER_DIE + cluster][t].base =
                    cluster_queue_base(die, cluster, (task_type_t)t);
            }
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
    if (gqm_push(g_cluster_queue[cluster_q][type].base, task)) {
        total_task_coord[task_id] = TASK_COORD(die_id, cluster_id);
        return true;
    }

    /* level 1: owning die queue of the given type (cluster full -> cluster unknown) */
    if (gqm_push(g_die_queue[die_id][type].base, task)) {
        total_task_coord[task_id] = TASK_COORD(die_id, TASK_COORD_INVALID);
        return true;
    }

    /* level 2: chip queue of the given type (die full -> die/cluster unknown) */
    if (gqm_push(g_chip_queue[type].base, task)) {
        total_task_coord[task_id] = TASK_COORD(TASK_COORD_INVALID, TASK_COORD_INVALID);
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
    if (gqm_pop(g_cluster_queue[cluster_q][type].base, task)) {
        total_task_coord[(uint32_t)*task] = TASK_COORD(die_id, cluster_id);
        return true;
    }

    /* level 1: owning die queue of the given type */
    if (gqm_pop(g_die_queue[die_id][type].base, task)) {
        total_task_coord[(uint32_t)*task] = TASK_COORD(die_id, cluster_id);
        return true;
    }

    /* level 2: chip queue of the given type */
    if (gqm_pop(g_chip_queue[type].base, task)) {
        total_task_coord[(uint32_t)*task] = TASK_COORD(die_id, cluster_id);
        return true;
    }

    return false;
}
