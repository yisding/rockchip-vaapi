#include "log.h"

#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define RK_LOG_MESSAGE_CAPACITY 4096

typedef enum {
    RK_LOG_FORMAT_TEXT,
    RK_LOG_FORMAT_JSON,
} RKLogFormat;

static FILE *log_file;
static RKLogLevel log_threshold = RK_LOG_LEVEL_INFO;
static RKLogFormat log_format = RK_LOG_FORMAT_TEXT;
static unsigned int log_users;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *level_name(RKLogLevel level)
{
    switch (level) {
    case RK_LOG_LEVEL_ERROR:   return "error";
    case RK_LOG_LEVEL_WARNING: return "warning";
    case RK_LOG_LEVEL_INFO:    return "info";
    case RK_LOG_LEVEL_DEBUG:   return "debug";
    case RK_LOG_LEVEL_TRACE:   return "trace";
    }
    return "unknown";
}

static RKLogLevel parse_level(const char *value)
{
    if (!value || !*value)
        return RK_LOG_LEVEL_INFO;
    if (!strcasecmp(value, "error"))
        return RK_LOG_LEVEL_ERROR;
    if (!strcasecmp(value, "warning") || !strcasecmp(value, "warn"))
        return RK_LOG_LEVEL_WARNING;
    if (!strcasecmp(value, "info"))
        return RK_LOG_LEVEL_INFO;
    if (!strcasecmp(value, "debug"))
        return RK_LOG_LEVEL_DEBUG;
    if (!strcasecmp(value, "trace"))
        return RK_LOG_LEVEL_TRACE;
    return RK_LOG_LEVEL_INFO;
}

static void open_log_file_locked(void)
{
    const char *path = getenv("RK_VAAPI_LOG");
    if (path && *path)
        log_file = fopen(path, "a");
    log_threshold = parse_level(getenv("RK_VAAPI_LOG_LEVEL"));
    const char *format = getenv("RK_VAAPI_LOG_FORMAT");
    log_format = format && !strcasecmp(format, "json")
               ? RK_LOG_FORMAT_JSON : RK_LOG_FORMAT_TEXT;
}

void rk_log_init(void)
{
    pthread_mutex_lock(&log_lock);
    if (log_users == 0)
        open_log_file_locked();
    if (log_users < UINT_MAX)
        log_users++;
    pthread_mutex_unlock(&log_lock);
}

void rk_log_finish(void)
{
    pthread_mutex_lock(&log_lock);
    if (log_users > 0)
        log_users--;
    if (log_users == 0 && log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_lock);
}

static void write_json_string(FILE *stream, const char *value)
{
    fputc('"', stream);
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor; cursor++) {
        switch (*cursor) {
        case '"':  fputs("\\\"", stream); break;
        case '\\': fputs("\\\\", stream); break;
        case '\b': fputs("\\b", stream); break;
        case '\f': fputs("\\f", stream); break;
        case '\n': fputs("\\n", stream); break;
        case '\r': fputs("\\r", stream); break;
        case '\t': fputs("\\t", stream); break;
        default:
            if (*cursor < 0x20)
                fprintf(stream, "\\u%04x", (unsigned int)*cursor);
            else
                fputc(*cursor, stream);
            break;
        }
    }
    fputc('"', stream);
}

static void write_text_message(FILE *stream, const char *message)
{
    for (const unsigned char *cursor = (const unsigned char *)message;
         *cursor; cursor++) {
        switch (*cursor) {
        case '\n': fputs("\\n", stream); break;
        case '\r': fputs("\\r", stream); break;
        case '\t': fputs("\\t", stream); break;
        default:
            if (*cursor < 0x20)
                fprintf(stream, "\\x%02x", (unsigned int)*cursor);
            else
                fputc(*cursor, stream);
            break;
        }
    }
}

static uint64_t realtime_nanoseconds(void)
{
    struct timespec timestamp;
    if (clock_gettime(CLOCK_REALTIME, &timestamp) != 0 ||
        timestamp.tv_sec < 0)
        return 0;
    uint64_t seconds = (uint64_t)timestamp.tv_sec;
    if (seconds > (UINT64_MAX - (uint64_t)timestamp.tv_nsec) / 1000000000u)
        return UINT64_MAX;
    return seconds * 1000000000u + (uint64_t)timestamp.tv_nsec;
}

static void format_message(char *message, size_t capacity,
                           const char *format, va_list arguments)
{
    int result = vsnprintf(message, capacity, format, arguments);
    if (result < 0) {
        (void)snprintf(message, capacity, "%s", "[format-error]");
        return;
    }
    if ((size_t)result < capacity)
        return;

    static const char suffix[] = "...[truncated]";
    size_t suffix_length = sizeof(suffix) - 1;
    if (capacity > suffix_length)
        memcpy(message + capacity - suffix_length - 1, suffix,
               suffix_length + 1);
}

void rk_log_message(RKLogLevel level, const char *source, int line,
                    const char *function, const char *format, ...)
{
    char message[RK_LOG_MESSAGE_CAPACITY];
    va_list arguments;
    va_start(arguments, format);
    format_message(message, sizeof(message), format, arguments);
    va_end(arguments);

    pthread_mutex_lock(&log_lock);
    if (!log_file || level > log_threshold) {
        pthread_mutex_unlock(&log_lock);
        return;
    }

    const char *safe_source = source ? source : "";
    const char *safe_function = function ? function : "";
    const char *name = level_name(level);
    uint64_t time_ns = realtime_nanoseconds();
    long thread_id = syscall(SYS_gettid);

    if (log_format == RK_LOG_FORMAT_JSON) {
        fprintf(log_file, "{\"time_unix_ns\":%llu,\"pid\":%ld,\"tid\":%ld,"
                "\"level\":",
                (unsigned long long)time_ns, (long)getpid(), thread_id);
        write_json_string(log_file, name);
        fputs(",\"source\":", log_file);
        write_json_string(log_file, safe_source);
        fprintf(log_file, ",\"line\":%d,\"function\":", line);
        write_json_string(log_file, safe_function);
        fputs(",\"message\":", log_file);
        write_json_string(log_file, message);
        fputs("}\n", log_file);
    } else {
        fprintf(log_file, "[rk-vaapi time_unix_ns=%llu pid=%ld tid=%ld "
                "level=%s source=%s line=%d function=%s] ",
                (unsigned long long)time_ns, (long)getpid(), thread_id, name,
                safe_source, line, safe_function);
        write_text_message(log_file, message);
        fputc('\n', log_file);
    }
    fflush(log_file);
    pthread_mutex_unlock(&log_lock);
}
