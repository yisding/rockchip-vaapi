#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

#define THREAD_COUNT 8
#define MESSAGES_PER_THREAD 100

typedef struct {
    int index;
} ThreadArguments;

static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *contents = calloc((size_t)length + 1, 1);
    if (!contents) {
        fclose(file);
        return NULL;
    }
    if (fread(contents, 1, (size_t)length, file) != (size_t)length) {
        free(contents);
        fclose(file);
        return NULL;
    }
    fclose(file);
    return contents;
}

static size_t line_count(const char *contents)
{
    size_t count = 0;
    for (const char *cursor = contents; *cursor; cursor++) {
        if (*cursor == '\n')
            count++;
    }
    return count;
}

static bool configure(const char *path, const char *level, const char *format)
{
    return setenv("RK_VAAPI_LOG", path, 1) == 0 &&
           setenv("RK_VAAPI_LOG_LEVEL", level, 1) == 0 &&
           setenv("RK_VAAPI_LOG_FORMAT", format, 1) == 0;
}

static bool test_filtering(const char *path)
{
    if (!configure(path, "warning", "text"))
        return false;
    rk_log_init();
    LOG_ERROR("filter-error");
    LOG_WARNING("filter-warning");
    LOG_INFO("filter-info");
    LOG_DEBUG("filter-debug");
    LOG_TRACE("filter-trace");
    rk_log_finish();

    char *contents = read_file(path);
    bool valid = contents && line_count(contents) == 2 &&
                 strstr(contents, "level=error") &&
                 strstr(contents, "filter-error") &&
                 strstr(contents, "level=warning") &&
                 strstr(contents, "filter-warning") &&
                 !strstr(contents, "filter-info") &&
                 !strstr(contents, "filter-debug") &&
                 !strstr(contents, "filter-trace");
    free(contents);
    return valid;
}

static bool test_json(const char *path)
{
    if (!configure(path, "trace", "json"))
        return false;
    rk_log_init();
    LOG_INFO("quoted \"value\" newline\nnext\tend");
    rk_log_finish();

    char *contents = read_file(path);
    bool valid = contents && line_count(contents) == 1 &&
                 contents[0] == '{' &&
                 strstr(contents, "\"time_unix_ns\":") &&
                 strstr(contents, "\"pid\":") &&
                 strstr(contents, "\"tid\":") &&
                 strstr(contents, "\"level\":\"info\"") &&
                 strstr(contents, "\"source\":\"tests/log_test.c\"") &&
                 strstr(contents, "\"function\":\"test_json\"") &&
                 strstr(contents,
                        "\"message\":\"quoted \\\"value\\\" "
                        "newline\\nnext\\tend\"") &&
                 strstr(contents, "}\n");
    free(contents);
    return valid;
}

static bool test_lifecycle(const char *first_path, const char *second_path)
{
    if (!configure(first_path, "info", "text"))
        return false;
    rk_log_init();
    rk_log_init();
    LOG_INFO("lifecycle-first");
    rk_log_finish();
    LOG_INFO("lifecycle-second");
    rk_log_finish();
    LOG_INFO("lifecycle-after-close");

    char *first = read_file(first_path);
    bool first_valid = first && line_count(first) == 2 &&
                       strstr(first, "lifecycle-first") &&
                       strstr(first, "lifecycle-second") &&
                       !strstr(first, "lifecycle-after-close");
    free(first);
    if (!first_valid || !configure(second_path, "info", "json"))
        return false;

    rk_log_init();
    LOG_INFO("lifecycle-reopened");
    rk_log_finish();
    char *second = read_file(second_path);
    bool second_valid = second && line_count(second) == 1 &&
                        strstr(second, "\"message\":\"lifecycle-reopened\"");
    free(second);
    return second_valid;
}

static void *thread_main(void *opaque)
{
    const ThreadArguments *arguments = opaque;
    for (int sequence = 0; sequence < MESSAGES_PER_THREAD; sequence++)
        LOG_DEBUG("thread=%d sequence=%d", arguments->index, sequence);
    return NULL;
}

static bool test_concurrency(const char *path)
{
    if (!configure(path, "debug", "text"))
        return false;
    rk_log_init();

    pthread_t threads[THREAD_COUNT];
    ThreadArguments arguments[THREAD_COUNT];
    int created = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        arguments[i].index = i;
        if (pthread_create(&threads[i], NULL, thread_main, &arguments[i]) != 0)
            break;
        created++;
    }
    for (int i = 0; i < created; i++)
        pthread_join(threads[i], NULL);
    rk_log_finish();
    if (created != THREAD_COUNT)
        return false;

    char *contents = read_file(path);
    if (!contents ||
        line_count(contents) != THREAD_COUNT * MESSAGES_PER_THREAD) {
        free(contents);
        return false;
    }
    bool valid = true;
    size_t records = 0;
    char *save = NULL;
    static const char prefix[] = "[rk-vaapi time_unix_ns=";
    for (char *line = strtok_r(contents, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        records++;
        if (strncmp(line, prefix, sizeof(prefix) - 1) != 0 ||
            !strstr(line, " level=debug source=tests/log_test.c ") ||
            !strstr(line, "] thread=")) {
            valid = false;
            break;
        }
    }
    free(contents);
    return valid && records == THREAD_COUNT * MESSAGES_PER_THREAD;
}

int main(void)
{
    char base[] = ".test-work.log-test.XXXXXX";
    int descriptor = mkstemp(base);
    if (descriptor < 0)
        return 1;
    close(descriptor);

    char json_path[PATH_MAX];
    char lifecycle_path[PATH_MAX];
    char reopen_path[PATH_MAX];
    char concurrency_path[PATH_MAX];
    if (snprintf(json_path, sizeof(json_path), "%s.json", base) < 0 ||
        snprintf(lifecycle_path, sizeof(lifecycle_path), "%s.lifecycle",
                 base) < 0 ||
        snprintf(reopen_path, sizeof(reopen_path), "%s.reopen", base) < 0 ||
        snprintf(concurrency_path, sizeof(concurrency_path), "%s.concurrent",
                 base) < 0) {
        unlink(base);
        return 1;
    }

    bool valid = test_filtering(base) &&
                 test_json(json_path) &&
                 test_lifecycle(lifecycle_path, reopen_path) &&
                 test_concurrency(concurrency_path);

    unlink(base);
    unlink(json_path);
    unlink(lifecycle_path);
    unlink(reopen_path);
    unlink(concurrency_path);
    unsetenv("RK_VAAPI_LOG");
    unsetenv("RK_VAAPI_LOG_LEVEL");
    unsetenv("RK_VAAPI_LOG_FORMAT");

    if (!valid) {
        fprintf(stderr, "logging tests: FAILED\n");
        return 1;
    }
    puts("logging tests: OK");
    return 0;
}
