#ifndef STATIC_SERVER_WORKER_H
#define STATIC_SERVER_WORKER_H

#include "http.h"
#include "logger.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define HEADER_LIMIT 16384

typedef enum { READING, WRITING_HEADERS, WRITING_FILE } State;

typedef struct Connection {
    int fd;
    int file_fd;
    State state;
    char input[HEADER_LIMIT + 1];
    size_t input_len;
    char output[2048];
    size_t output_len, output_pos;
    char *file_buffer;
    size_t file_used, file_pos;
    uint64_t remaining, sent_body;
    int status;
    bool keep_alive, head;
    char method[32];
    char target[TARGET_LIMIT + 1];
    double last_activity, request_start;
    struct Connection *next;
} Connection;

typedef struct Pending {
    int fd;
    struct Pending *next;
} Pending;

typedef struct Worker {
    pthread_t thread;
    int wake[2];
    pthread_mutex_t queue_mutex;
    Pending *queue_head, *queue_tail;
    Connection *clients;
    int root_fd;
    Logger *logger;
    atomic_int *active;
    atomic_int load;
} Worker;

extern atomic_bool stopping;

Worker *worker_create(int root_fd, Logger *logger, atomic_int *active);
int worker_load(const Worker *worker);
int worker_enqueue(Worker *worker, int fd);
void worker_notify(Worker *worker);
void worker_join_destroy(Worker *worker);

#endif
