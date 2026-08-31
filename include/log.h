/*
 * log.h - Lightweight logging macros for scheduler and algorithm subsystems
 *
 * Compile-time switch (see conf.h):
 *   WORKER_LOG = 1 -> compile in WORKER_LOGF() worker-thread logging
 *   WORKER_LOG = 0 -> WORKER_LOGF() expands to ((void)0), no logging code
 *
 * Runtime (WORKER_LOG only): g_worker_log toggles output, g_log_output_mode
 * selects destination (0=file, 1=stdout, 2=both, default LOG_OUTPUT_MODE).
 */

#ifndef SCHEDULER_LOG_H
#define SCHEDULER_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "conf.h"

#define LOG_MAX_THREADS 16

/* ── Worker-thread logging ───────────────────────────────────────────── */

#if WORKER_LOG
extern int g_worker_log;
extern int g_log_output_mode;

void log_init(const char *base_filename);
void log_close(void);

/* Internal: forwarded by WORKER_LOGF macro */
void log_write(const char *file, int line, const char *fmt, ...);

/* Variadic dispatch helpers (0–5 args) */
#define _LOG_WRITE_0(file, line, fmt) \
    log_write(file, line, fmt)
#define _LOG_WRITE_1(file, line, fmt, a1) \
    log_write(file, line, fmt, a1)
#define _LOG_WRITE_2(file, line, fmt, a1, a2) \
    log_write(file, line, fmt, a1, a2)
#define _LOG_WRITE_3(file, line, fmt, a1, a2, a3) \
    log_write(file, line, fmt, a1, a2, a3)
#define _LOG_WRITE_4(file, line, fmt, a1, a2, a3, a4) \
    log_write(file, line, fmt, a1, a2, a3, a4)
#define _LOG_WRITE_5(file, line, fmt, a1, a2, a3, a4, a5) \
    log_write(file, line, fmt, a1, a2, a3, a4, a5)
#define _LOG_WRITE_GET(_0, _1, _2, _3, _4, _5, NAME, ...) NAME

/* WORKER_LOGF — worker-thread CSV/log output
 *
 * Output format: "[<file>:<line>] <fmt...>\\n"
 * Thread-safe: protected by an internal mutex inside log_write().
 * Enabled at runtime when g_worker_log != 0. */
#define WORKER_LOGF(...)                                                   \
    do {                                                                    \
        if (g_worker_log) {                                                 \
            _LOG_WRITE_GET(__VA_ARGS__, _LOG_WRITE_5, _LOG_WRITE_4,        \
                           _LOG_WRITE_3, _LOG_WRITE_2, _LOG_WRITE_1,       \
                           _LOG_WRITE_0)(__FILE__, __LINE__, __VA_ARGS__);  \
        }                                                                   \
    } while (0)

#else
#define WORKER_LOGF(...) ((void)0)
#endif /* WORKER_LOG */

#endif /* SCHEDULER_LOG_H */
