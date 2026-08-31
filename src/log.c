/*
 * log.c - Lightweight worker-thread logging implementation for WORKER_LOGF().
 *
 * Compiled only when WORKER_LOG (conf.h) is enabled. The WORKER_LOGF() macro
 * in log.h already guards each call site with g_worker_log at runtime; this
 * translation unit provides g_worker_log, the destination selection, and the
 * mutex-protected log_write() backend.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "conf.h"
#include "log.h"

#if WORKER_LOG

/* Runtime toggle checked by WORKER_LOGF(). Defaults to enabled so that the
 * compile-time WORKER_LOG switch alone decides whether logging is emitted. */
int g_worker_log = 1;

/* Destination: 0=file, 1=stdout, 2=both (default from conf.h LOG_OUTPUT_MODE). */
int g_log_output_mode = LOG_OUTPUT_MODE;

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_file = NULL;

void log_init(const char *base_filename)
{
    if (base_filename == NULL) {
        return;
    }

    if (g_log_output_mode == 0 || g_log_output_mode == 2) {
        char path[320];
        snprintf(path, sizeof(path), "%s.log", base_filename);
        g_log_file = fopen(path, "w");
    }
}

void log_close(void)
{
    if (g_log_file != NULL) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

void log_write(const char *file, int line, const char *fmt, ...)
{
    va_list args;
    char msg[512];

    const char *base = strrchr(file, '/');
    if (base != NULL) {
        base++;
    } else {
        base = file;
    }

    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    pthread_mutex_lock(&g_log_mutex);

    if (g_log_output_mode != 0) {           /* 1=stdout, 2=both */
        fprintf(stdout, "[%s:%d] %s\n", base, line, msg);
        fflush(stdout);
    }
    if (g_log_file != NULL) {               /* 0=file, 2=both */
        fprintf(g_log_file, "[%s:%d] %s\n", base, line, msg);
        fflush(g_log_file);
    }

    pthread_mutex_unlock(&g_log_mutex);
}

#endif /* WORKER_LOG */
