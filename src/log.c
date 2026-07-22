#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static FILE *log_file;
static pthread_once_t log_once = PTHREAD_ONCE_INIT;

static void open_log_file(void)
{
    const char *path = getenv("RK_VAAPI_LOG");
    if (path && *path)
        log_file = fopen(path, "a");
}

void rk_log_init(void)
{
    (void)pthread_once(&log_once, open_log_file);
}

void rk_log_message(const char *format, ...)
{
    if (!log_file)
        return;

    flockfile(log_file);
    fprintf(log_file, "[rk-vaapi pid=%d] ", getpid());
    va_list arguments;
    va_start(arguments, format);
    // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) -- va_start above.
    vfprintf(log_file, format, arguments);
    va_end(arguments);
    fputc('\n', log_file);
    funlockfile(log_file);
}
