#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include "logger.h"

static FILE           *log_fp    = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_init(const char *path) {
    log_fp = fopen(path, "a");
    if (!log_fp) {
        perror("log_init: fopen");
        return;
    }
    log_write("=== server started ===");
}

void log_write(const char *fmt, ...) {
    if (!log_fp) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    pthread_mutex_lock(&log_mutex);
    fprintf(log_fp, "[%s] ", ts);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);

    fputc('\n', log_fp);
    fflush(log_fp);
    pthread_mutex_unlock(&log_mutex);
}

void log_close(void) {
    pthread_mutex_lock(&log_mutex);
    if (log_fp) {
        log_write("=== server stopped ===");
        fclose(log_fp);
        log_fp = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}
