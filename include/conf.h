#ifndef SCHEDULER_CONF_H
#define SCHEDULER_CONF_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

/* Cache line size: hot shared atomics are aligned to and padded to a full
 * 64-byte cache line so each one owns a private line (avoids false sharing). */
#define CACHE_LINE_SIZE 64

typedef struct { _Alignas(CACHE_LINE_SIZE) atomic_bool   v; } cacheline_bool_t;
typedef struct { _Alignas(CACHE_LINE_SIZE) atomic_int    v; } cacheline_int_t;
typedef struct { _Alignas(CACHE_LINE_SIZE) _Atomic uint64_t v; } cacheline_u64_t;
typedef struct { _Alignas(CACHE_LINE_SIZE) atomic_flag   v; } cacheline_flag_t;

extern cacheline_int_t g_start_barrier;

#define AIC_OSTD 2
#define AIC_CNT 64

#define AIC_CNT_PER_DIE 32
#define EXE_TYPE_CNT 2

#define MAX_TASK_CNT 4096
extern uint32_t g_pred_xor[MAX_TASK_CNT];

#define RQ_SIZE 4096
#define RQ_BATCH_SIZE 512

#define CQ_SIZE 4096
#define CQ_BATCH_SIZE 512
#define CQ_MASK (CQ_SIZE - 1)

#ifndef PAINTER_THREAD_CNT
#define PAINTER_THREAD_CNT 4
#endif
#ifndef DISPATCH_THREAD_CNT
#define DISPATCH_THREAD_CNT 1
#endif
#define AIC_CNT_PER_THREAD 60

/* Compile-time switch: 1 = compile in WORKER_LOGF() worker logging, 0 = strip it out.
 * Default is 0 (off); enable with `make LOG=1` or -DWORKER_LOG=1. */
#ifndef WORKER_LOG
#define WORKER_LOG 0
#endif

/* Log output mode: 0=file, 1=stdout, 2=both */
#define LOG_OUTPUT_MODE 2

#endif /* SCHEDULER_CONF_H */
