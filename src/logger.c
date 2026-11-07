#include "logger.h"

#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_QUEUE_LIMIT 4096

static bool inside_root(const char *root, const char *path) {
    size_t length = strlen(root);
    if (length == 1 && root[0] == '/') {
        return true;
    }
    return !strncmp(root, path, length) && (path[length] == '/' || path[length] == '\0');
}

static LogMessage *pop_message(Logger *log) {
    LogMessage *message = log->head;
    log->head = message->next;
    if (!log->head) {
        log->tail = NULL;
    }
    --log->size;
    return message;
}

// Последовательно пишет сообщения и перед завершением опустошает очередь
static void *logger_main(void *argument) {
    Logger *log = argument;
    for (;;) {
        pthread_mutex_lock(&log->mutex);
        // После запрета приёма поток завершается только с пустой очередью
        while (!log->head && log->accepting) {
            pthread_cond_wait(&log->cond, &log->mutex);
        }
        if (!log->head && !log->accepting) {
            pthread_mutex_unlock(&log->mutex);
            break;
        }
        LogMessage *message = pop_message(log);
        pthread_mutex_unlock(&log->mutex);

        // Медленная запись не удерживает mutex производителей
        fputs(message->text, log->file);
        fputc('\n', log->file);
        fflush(log->file);
        free(message->text);
        free(message);
    }
    fflush(log->file);
    fclose(log->file);
    log->file = NULL;
    return NULL;
}

// Открывает один файл журнала и запускает единственный поток записи
int logger_open(Logger *log, const char *requested, const char *root) {
    memset(log, 0, sizeof(*log));
    char *copy = strdup(requested);
    if (!copy) {
        return -1;
    }
    char *slash = strrchr(copy, '/');
    const char *name = slash ? slash + 1 : copy;
    if (!*name) {
        free(copy);
        errno = EINVAL;
        return -1;
    }
    char *directory = ".";
    if (slash) {
        *slash = '\0';
        directory = slash == copy ? "/" : copy;
    }
    if (!strcmp(directory, "logs")) {
        mkdir(directory, 0755);
    }
    char *parent = realpath(directory, NULL);
    if (!parent) {
        free(copy);
        return -1;
    }
    size_t length = strlen(parent) + strlen(name) + 2;
    char *path = malloc(length);
    if (!path) {
        free(parent);
        free(copy);
        return -1;
    }
    snprintf(path, length, "%s/%s", parent, name);
    char *existing = realpath(path, NULL);
    bool forbidden = inside_root(root, existing ? existing : path);
    if (!forbidden) {
        log->file = fopen(path, "a");
    }
    free(existing);
    free(path);
    free(parent);
    free(copy);
    if (forbidden) {
        errno = EACCES;
        return -1;
    }
    if (!log->file) {
        return -1;
    }
    int error = pthread_mutex_init(&log->mutex, NULL);
    if (error) {
        fclose(log->file);
        log->file = NULL;
        errno = error;
        return -1;
    }
    error = pthread_cond_init(&log->cond, NULL);
    if (error) {
        pthread_mutex_destroy(&log->mutex);
        fclose(log->file);
        log->file = NULL;
        errno = error;
        return -1;
    }
    log->accepting = true;
    error = pthread_create(&log->thread, NULL, logger_main, log);
    if (error) {
        log->accepting = false;
        pthread_cond_destroy(&log->cond);
        pthread_mutex_destroy(&log->mutex);
        fclose(log->file);
        log->file = NULL;
        errno = error;
        return -1;
    }
    log->thread_started = true;
    return 0;
}

// Запрещает новые сообщения и ждёт записи всего остатка очереди
void logger_close(Logger *log) {
    if (!log || !log->thread_started) {
        return;
    }
    pthread_mutex_lock(&log->mutex);
    log->accepting = false;
    pthread_cond_signal(&log->cond);
    pthread_mutex_unlock(&log->mutex);
    pthread_join(log->thread, NULL);
    log->thread_started = false;
    pthread_cond_destroy(&log->cond);
    pthread_mutex_destroy(&log->mutex);
}

void log_event(Logger *log, const char *format, ...) {
    time_t stamp = time(NULL);
    struct tm tm_value = {0};
    char text[32] = "1970-01-01T00:00:00+0000";
    if (localtime_r(&stamp, &tm_value)) {
        strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S%z", &tm_value);
    }

    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int body_length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (body_length < 0) {
        va_end(args);
        pthread_mutex_lock(&log->mutex);
        ++log->dropped;
        pthread_mutex_unlock(&log->mutex);
        return;
    }
    size_t timestamp_length = strlen(text);
    size_t length = timestamp_length + 1 + (size_t)body_length + 1;
    LogMessage *message = malloc(sizeof(*message));
    char *ready = malloc(length);
    if (!message || !ready) {
        free(message);
        free(ready);
        va_end(args);
        pthread_mutex_lock(&log->mutex);
        ++log->dropped;
        pthread_mutex_unlock(&log->mutex);
        return;
    }
    memcpy(ready, text, timestamp_length);
    ready[timestamp_length] = ' ';
    vsnprintf(ready + timestamp_length + 1, (size_t)body_length + 1, format, args);
    va_end(args);

    message->text = ready;
    message->next = NULL;
    pthread_mutex_lock(&log->mutex);
    // Переполнение не должно останавливать HTTP-поток
    if (!log->accepting || log->size >= LOG_QUEUE_LIMIT) {
        ++log->dropped;
        pthread_mutex_unlock(&log->mutex);
        free(message->text);
        free(message);
        return;
    }
    if (log->tail) {
        log->tail->next = message;
    } else {
        log->head = message;
    }
    log->tail = message;
    ++log->size;
    pthread_cond_signal(&log->cond);
    pthread_mutex_unlock(&log->mutex);
}

size_t logger_dropped(Logger *log) {
    pthread_mutex_lock(&log->mutex);
    size_t dropped = log->dropped;
    pthread_mutex_unlock(&log->mutex);
    return dropped;
}

