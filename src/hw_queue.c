#include "hw_queue.h"

/* ---------------------------------------------------------------------
 * three-level queue instances, mirroring the hardware TOPO bottom-up:
 *   12 cluster queues + 2 die queues + 1 chip queue
 * --------------------------------------------------------------------- */
task_queue_desc_t g_chip_queue[CHIP_QUEUE_NUM];
task_queue_desc_t g_die_queue[DIE_QUEUE_NUM];
task_queue_desc_t g_cluster_queue[CLUSTER_QUEUE_NUM];

/* build the three-level queues bottom-up */
void queue_init(void)
{
    uint8_t q = 0;

    /* level 0: cluster queues, 6 * 2 = 12 */
    for (uint8_t die = 0; die < DIE_NUM; die++) {
        for (uint8_t cluster = 0; cluster < CLUSTER_PER_DIE; cluster++) {
            g_cluster_queue[q].base       = cluster_queue_base(die, cluster);
            q++;
        }
    }

    /* level 1: die queues, 2 */
    for (uint8_t die = 0; die < DIE_NUM; die++) {
        g_die_queue[die].base       = die_queue_base(die);
    }

    /* level 2: chip queue, 1 */
    g_chip_queue[0].base       = chip_queue_base();
}

/* ---------------------------------------------------------------------
 * PUSH / POP by continuous core_id.
 * the owning die/cluster is resolved from core_id; the task is first routed
 * to the nearest cluster queue, and on failure it falls back to the owning
 * die queue and finally the chip queue (bottom-up levels).
 * --------------------------------------------------------------------- */

bool queue_push(uint32_t core_id, uint64_t task)
{
    uint8_t die_id     = core_id_to_die(core_id);
    uint8_t cluster_id = core_id_to_cluster(core_id);

    /* level 0: nearest cluster queue */
    if (gqm_push(&g_cluster_queue[die_id * CLUSTER_PER_DIE + cluster_id], task)) {
        return true;
    }

    /* level 1: owning die queue */
    if (gqm_push(&g_die_queue[die_id], task)) {
        return true;
    }

    /* level 2: chip queue */
    return gqm_push(&g_chip_queue[0], task);
}

bool queue_pop(uint32_t core_id, uint64_t *task)
{
    uint8_t die_id     = core_id_to_die(core_id);
    uint8_t cluster_id = core_id_to_cluster(core_id);

    /* level 0: nearest cluster queue */
    if (gqm_pop(&g_cluster_queue[die_id * CLUSTER_PER_DIE + cluster_id], task)) {
        return true;
    }

    /* level 1: owning die queue */
    if (gqm_pop(&g_die_queue[die_id], task)) {
        return true;
    }

    /* level 2: chip queue */
    return gqm_pop(&g_chip_queue[0], task);
}
