#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

#include "log.h"

static pthread_mutex_t LOG_MTX = PTHREAD_MUTEX_INITIALIZER;

static void log_write(const char* level, const char* fmt, va_list ap) {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_info);

    pthread_mutex_lock(&LOG_MTX);
    fprintf(stderr, "[%s] [%s] ", ts, level);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    pthread_mutex_unlock(&LOG_MTX);
}

void log_info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write("WARN", fmt, ap);
    va_end(ap);
}

void log_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write("ERROR", fmt, ap);
    va_end(ap);
}
