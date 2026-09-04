/*
 * cutter.h - Dependency resolution worker
 */

#ifndef PAINTER_H
#define PAINTER_H

#include <stdatomic.h>

#include "conf.h"
#include "dispatch.h"

#include "task.h"
#include "queue.h"


/* Extern declarations from ring_buf / task system (avoid pulling in algorithm/ring_buf.h
 * which conflicts with common/queue.h) */
extern atomic_int g_task_id;

void *painter(void *arg);
void init_queue(void);

#endif
