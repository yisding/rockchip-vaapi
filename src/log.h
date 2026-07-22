#ifndef RK_VAAPI_LOG_H
#define RK_VAAPI_LOG_H

void rk_log_init(void);
void rk_log_message(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

#define LOG(...) rk_log_message(__VA_ARGS__)

#endif
