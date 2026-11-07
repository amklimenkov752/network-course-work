#ifndef STATIC_SERVER_LOGGER_H
#define STATIC_SERVER_LOGGER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct LogMessage {
    char *text;
    struct LogMessage *next;
} LogMessage;

typedef struct {
    FILE *file;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    LogMessage *head;
    LogMessage *tail;
    size_t size;
    size_t dropped;
    bool accepting;
    bool thread_started;
} Logger;

int logger_open(Logger *log, const char *path, const char *static_root);
void logger_close(Logger *log);
void log_event(Logger *log, const char *format, ...);
size_t logger_dropped(Logger *log);

#endif
