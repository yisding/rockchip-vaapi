#ifndef RK_VAAPI_LOG_H
#define RK_VAAPI_LOG_H

typedef enum {
    RK_LOG_LEVEL_ERROR = 0,
    RK_LOG_LEVEL_WARNING,
    RK_LOG_LEVEL_INFO,
    RK_LOG_LEVEL_DEBUG,
    RK_LOG_LEVEL_TRACE,
} RKLogLevel;

void rk_log_init(void);
void rk_log_finish(void);
void rk_log_message(RKLogLevel level, const char *source, int line,
                    const char *function, const char *format, ...)
    __attribute__((format(printf, 5, 6)));

#define RK_LOG(level, ...) \
    rk_log_message((level), __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_ERROR(...) RK_LOG(RK_LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_WARNING(...) RK_LOG(RK_LOG_LEVEL_WARNING, __VA_ARGS__)
#define LOG_INFO(...) RK_LOG(RK_LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_DEBUG(...) RK_LOG(RK_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define LOG_TRACE(...) RK_LOG(RK_LOG_LEVEL_TRACE, __VA_ARGS__)

/* Preserve the existing call-site API at the informational level. */
#define LOG(...) LOG_INFO(__VA_ARGS__)

#endif
