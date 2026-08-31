#ifndef SCHEDULER_CONF_H
#define SCHEDULER_CONF_H

#include <pthread.h>
#include <stdatomic.h>

extern atomic_int g_start_barrier;

#define AIC_OSTD 2
#define AIC_CNT 64

#define AIC_CNT_PER_DIE 32
#define EXE_TYPE_CNT 2

#define RQ_SIZE 1024
#define RQ_BATCH_SIZE 512

#define CQ_BATCH_SIZE 512
#define CQ_MASK (CQ_SIZE - 1)
#define CQ_SIZE 4096

#ifndef PAINTER_THREAD_CNT
#define PAINTER_THREAD_CNT 4
#endif
#ifndef DISPATCH_THREAD_CNT
#define DISPATCH_THREAD_CNT 1
#endif
#define AIC_CNT_PER_THREAD 32

/* Compile-time switch: 1 = compile in WORKER_LOGF() worker logging, 0 = strip it out.
 * Default is 0 (off); enable with `make LOG=1` or -DWORKER_LOG=1. */
#ifndef WORKER_LOG
#define WORKER_LOG 0
#endif

/* Log output mode: 0=file, 1=stdout, 2=both */
#define LOG_OUTPUT_MODE 2

#endif /* SCHEDULER_CONF_H */
