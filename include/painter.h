/*
 * cutter.h - Dependency resolution worker
 */

#ifndef PAINTER_H
#define PAINTER_H

#include <stdatomic.h>

#include "conf.h"
#include "dispatch.h"

#ifndef SCHEDULER_CASE
#define SCHEDULER_CASE cases/qwen3_14b_decode_subgraph.h
#endif

/* Macro to stringify the include directive properly */
#define __INCLUDE(x) #x
#define _INCLUDE_FILE(x) __INCLUDE(x)
#include _INCLUDE_FILE(SCHEDULER_CASE)

#include "task.h"
#include "queue.h"


/* Extern declarations from ring_buf / task system (avoid pulling in algorithm/ring_buf.h
 * which conflicts with common/queue.h) */
extern atomic_int g_task_id;


void *painter(void *arg);
void seed_source_tasks(void);

#endif
