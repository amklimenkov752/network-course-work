#include "worker.h"
#include "http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FILE_CHUNK 65536
#define IDLE_SECONDS 30
#define SHUTDOWN_DRAIN_SECONDS 10

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1000000000.0;
}

static void log_request(Worker *worker, Connection *client) {
    double elapsed_ms = (now_seconds() - client->request_start) * 1000.0;
    log_event(worker->logger, "fd=%d method=%s target=%s status=%d body_bytes=%" PRIu64 " duration_ms=%.3f",
              client->fd, client->method, client->target, client->status, client->sent_body, elapsed_ms);
}

static void respond(Connection *client, int status, const char *type, uint64_t length, int file_fd) {
    client->status = status;
    client->file_fd = file_fd;
    client->remaining = length;
    client->sent_body = 0;
    client->file_pos = client->file_used = client->output_pos = 0;
    char date[64] = "Thu, 01 Jan 1970 00:00:00 GMT";
    time_t now = time(NULL);
    struct tm utc;
    if (now != (time_t)-1 && gmtime_r(&now, &utc)) {
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    }
    int count = snprintf(client->output, sizeof(client->output),
                         "HTTP/1.1 %d %s\r\nContent-Length: %" PRIu64 "\r\n"
                         "Content-Type: %s\r\nDate: %s\r\nX-Content-Type-Options: nosniff\r\n"
                         "Connection: %s\r\n%s\r\n",
                         status, reason(status), length, type, date,
                         client->keep_alive ? "keep-alive" : "close",
                         status == 405 ? "Allow: GET, HEAD\r\n" : "");
    client->output_len = count > 0 && (size_t)count < sizeof(client->output) ? (size_t)count : 0;
    client->state = WRITING_HEADERS;
}

static void error_response(Connection *client, int status) {
    char body[80];
    int length = snprintf(body, sizeof(body), "%d %s\n", status, reason(status));
    client->keep_alive = false;
    respond(client, status, "text/plain; charset=utf-8", (uint64_t)length, -1);
    if (!client->head && client->output_len + (size_t)length < sizeof(client->output)) {
        memcpy(client->output + client->output_len, body, (size_t)length);
        client->output_len += (size_t)length;
        client->sent_body = (uint64_t)length;
    }
    client->remaining = 0;
}

static size_t header_end(const char *text, size_t length) {
    for (size_t i = 0; i + 3 < length; ++i) {
        if (text[i] == '\r' && text[i + 1] == '\n' && text[i + 2] == '\r' && text[i + 3] == '\n') {
            return i;
        }
    }
    return SIZE_MAX;
}

static char *trim(char *text) {
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    size_t length = strlen(text);
    while (length && (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[--length] = '\0';
    }
    return text;
}

static void lowercase(char *text) {
    for (; *text; ++text) {
        *text = (char)tolower((unsigned char)*text);
    }
}

static bool is_field_name_char(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9')) {
        return true;
    }
    return strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static bool valid_port(const char *port) {
    if (!*port) {
        return false;
    }
    unsigned number = 0;
    for (const unsigned char *p = (const unsigned char *)port; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        if (number > 6553 || (number == 6553 && *p - '0' > 5)) {
            return false;
        }
        number = number * 10 + (*p - '0');
    }
    return true;
}

static bool valid_host(const char *value) {
    if (!value || !*value) {
        return false;
    }
    if (*value == '[') {
        const char *end = strchr(value, ']');
        if (!end || end == value + 1 || (size_t)(end - value - 1) >= INET6_ADDRSTRLEN) {
            return false;
        }
        char address[INET6_ADDRSTRLEN];
        memcpy(address, value + 1, (size_t)(end - value - 1));
        address[end - value - 1] = '\0';
        struct in6_addr parsed;
        if (inet_pton(AF_INET6, address, &parsed) != 1) {
            return false;
        }
        return !end[1] || (end[1] == ':' && valid_port(end + 2));
    }
    const char *port = strchr(value, ':');
    size_t host_length = port ? (size_t)(port - value) : strlen(value);
    if (!host_length || (port && !valid_port(port + 1))) {
        return false;
    }
    for (size_t i = 0; i < host_length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || strchr("-._~!$&'()*+,;=", c)) {
            continue;
        }
        if (c == '%' && i + 2 < host_length && isxdigit((unsigned char)value[i + 1]) &&
            isxdigit((unsigned char)value[i + 2])) {
            i += 2;
            continue;
        }
        return false;
    }
    return true;
}

// Ноль означает готовый ответ, остальные значения — HTTP-статус ошибки
static int prepare_response(Worker *worker, Connection *client, char *header) {
    char *save = NULL;
    char *line = strtok_r(header, "\r\n", &save);
    if (!line) {
        return 400;
    }
    char *first_space = strchr(line, ' ');
    if (!first_space) {
        return 400;
    }
    *first_space++ = '\0';
    char *second_space = strchr(first_space, ' ');
    if (!second_space) {
        return 400;
    }
    *second_space++ = '\0';
    if (strchr(second_space, ' ') || strlen(line) >= sizeof(client->method)) {
        return 400;
    }
    strcpy(client->method, line);
    client->head = strcmp(line, "HEAD") == 0;
    if (strlen(first_space) > TARGET_LIMIT) {
        return 414;
    }
    strcpy(client->target, first_space);
    if (strcmp(second_space, "HTTP/1.1") && strcmp(second_space, "HTTP/1.0")) {
        return 400;
    }
    client->keep_alive = strcmp(second_space, "HTTP/1.1") == 0;
    int host_count = 0;
    bool body = false;
    while ((line = strtok_r(NULL, "\r\n", &save))) {
        char *colon = strchr(line, ':');
        if (!colon || colon == line) {
            return 400;
        }
        for (char *p = line; p < colon; ++p) {
            if (!is_field_name_char((unsigned char)*p)) {
                return 400;
            }
        }
        *colon++ = '\0';
        lowercase(line);
        char *value = trim(colon);
        for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
            if ((*p < 32 && *p != '\t') || *p == 127) {
                return 400;
            }
        }
        if (!strcmp(line, "host")) {
            ++host_count;
            if (host_count > 1 || !valid_host(value)) {
                return 400;
            }
        }
        if (!strcmp(line, "connection")) {
            lowercase(value);
            if (strstr(value, "close")) {
                client->keep_alive = false;
            } else if (strstr(value, "keep-alive")) {
                client->keep_alive = true;
            }
        }
        if (!strcmp(line, "transfer-encoding") ||
            (!strcmp(line, "content-length") && strcmp(value, "0"))) {
            body = true;
        }
    }
    if (!strcmp(second_space, "HTTP/1.1") && !host_count) {
        return 400;
    }
    if (strcmp(client->method, "GET") && strcmp(client->method, "HEAD")) {
        return 405;
    }
    if (body) {
        return 400;
    }
    char path[TARGET_LIMIT + 1];
    int status = decode_path(client->target, path);
    if (status != 200) {
        return status;
    }
    uint64_t size = 0;
    char *name = NULL;
    int file_fd = open_static(worker->root_fd, path, &size, &name);
    if (file_fd < 0) {
        return -file_fd;
    }
    const char *content_type = mime_type(name)->content_type;
    if (!client->head && size && !client->file_buffer) {
        client->file_buffer = malloc(FILE_CHUNK);
        if (!client->file_buffer) {
            close(file_fd);
            return 500;
        }
    }
    respond(client, 200, content_type, size, file_fd);
    return 0;
}

static void parse_request(Worker *worker, Connection *client) {
    size_t end = header_end(client->input, client->input_len);
    if (end == SIZE_MAX) {
        if (client->input_len >= HEADER_LIMIT) {
            error_response(client, 431);
        }
        return;
    }
    char *header = malloc(end + 3);
    if (!header) {
        error_response(client, 500);
        return;
    }
    memcpy(header, client->input, end + 2);
    header[end + 2] = '\0';
    size_t consumed = end + 4;
    memmove(client->input, client->input + consumed, client->input_len - consumed);
    client->input_len -= consumed;
    client->request_start = now_seconds();
    for (size_t i = 0; i < end + 2; ++i) {
        if ((header[i] == '\r' && (i + 1 >= end + 2 || header[i + 1] != '\n')) ||
            (header[i] == '\n' && (i == 0 || header[i - 1] != '\r'))) {
            error_response(client, 400);
            free(header);
            return;
        }
    }
    int status = prepare_response(worker, client, header);
    free(header);
    if (status) {
        error_response(client, status);
    }
}

static bool read_socket(Worker *worker, Connection *client) {
    while (client->input_len < HEADER_LIMIT) {
        ssize_t count = recv(client->fd, client->input + client->input_len,
                             HEADER_LIMIT - client->input_len, 0);
        if (count > 0) {
            if (memchr(client->input + client->input_len, '\0', (size_t)count)) {
                error_response(client, 400);
                return true;
            }
            client->input_len += (size_t)count;
            client->last_activity = now_seconds();
            if (header_end(client->input, client->input_len) != SIZE_MAX) {
                parse_request(worker, client);
                return true;
            }
        } else if (count == 0) {
            return false;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        } else if (errno != EINTR) {
            return false;
        }
    }
    error_response(client, 431);
    return true;
}

static bool finish_response(Worker *worker, Connection *client) {
    log_request(worker, client);
    if (client->file_fd >= 0) {
        close(client->file_fd);
    }
    client->file_fd = -1;
    if (!client->keep_alive) {
        return false;
    }
    client->state = READING;
    client->head = false;
    client->method[0] = client->target[0] = '\0';
    client->last_activity = now_seconds();
    if (client->input_len) {
        parse_request(worker, client);
    }
    return true;
}

// Продвигает автомат ответа, сохраняя позиции между частичными вызовами send()
static bool write_socket(Worker *worker, Connection *client) {
    for (int steps = 0; steps < 4; ++steps) {
        if (client->state == WRITING_HEADERS) {
            ssize_t count = send(client->fd, client->output + client->output_pos,
                                 client->output_len - client->output_pos, 0);
            if (count > 0) {
                client->output_pos += (size_t)count;
                client->last_activity = now_seconds();
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            } else {
                return false;
            }
            if (client->output_pos < client->output_len) {
                return true;
            }
            if (client->file_fd < 0 || client->head || !client->remaining) {
                return finish_response(worker, client);
            }
            client->state = WRITING_FILE;
        }
        if (client->state == WRITING_FILE) {
            if (!client->file_buffer) {
                client->file_buffer = malloc(FILE_CHUNK);
            }
            if (!client->file_buffer) {
                return false;
            }
            if (client->file_pos == client->file_used) {
                // Счётчик remaining ограничивает тело зафиксированным Content-Length
                size_t want = client->remaining < FILE_CHUNK ? (size_t)client->remaining : FILE_CHUNK;
                ssize_t count = read(client->file_fd, client->file_buffer, want);
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count <= 0) {
                    if (count == 0) {
                        log_event(worker->logger,
                                  "file ended before Content-Length fd=%d expected=%" PRIu64
                                  " sent=%" PRIu64,
                                  client->fd, client->sent_body + client->remaining,
                                  client->sent_body);
                    } else {
                        log_event(worker->logger, "file read failed fd=%d: %s",
                                  client->fd, strerror(errno));
                    }
                    return false;
                }
                client->file_used = (size_t)count;
                client->file_pos = 0;
            }
            ssize_t count = send(client->fd, client->file_buffer + client->file_pos,
                                 client->file_used - client->file_pos, 0);
            if (count > 0) {
                client->file_pos += (size_t)count;
                client->remaining -= (uint64_t)count;
                client->sent_body += (uint64_t)count;
                client->last_activity = now_seconds();
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            } else {
                return false;
            }
            if (!client->remaining) {
                return finish_response(worker, client);
            }
            if (client->file_pos < client->file_used) {
                return true;
            }
        }
    }
    return true;
}

static void remove_client(Worker *worker, Connection **position) {
    Connection *client = *position;
    *position = client->next;
    if (client->file_fd >= 0) {
        close(client->file_fd);
    }
    close(client->fd);
    free(client->file_buffer);
    free(client);
    atomic_fetch_sub(worker->active, 1);
    atomic_fetch_sub(&worker->load, 1);
}

// Отделяет общую очередь и передаёт все её соединения текущему worker
static void add_pending(Worker *worker) {
    char bytes[256];
    while (read(worker->wake[0], bytes, sizeof(bytes)) > 0) {
    }
    pthread_mutex_lock(&worker->queue_mutex);
    Pending *pending = worker->queue_head;
    worker->queue_head = worker->queue_tail = NULL;
    pthread_mutex_unlock(&worker->queue_mutex);
    while (pending) {
        Pending *next = pending->next;
        if (stopping) {
            close(pending->fd);
            atomic_fetch_sub(worker->active, 1);
            atomic_fetch_sub(&worker->load, 1);
        } else {
            Connection *client = calloc(1, sizeof(*client));
            if (!client) {
                close(pending->fd);
                atomic_fetch_sub(worker->active, 1);
                atomic_fetch_sub(&worker->load, 1);
            } else {
                client->fd = pending->fd;
                client->file_fd = -1;
                client->state = READING;
                client->last_activity = client->request_start = now_seconds();
                client->next = worker->clients;
                worker->clients = client;
            }
        }
        free(pending);
        pending = next;
    }
}

// Обслуживает несколько соединений одним select и единолично меняет их состояние
static void *worker_main(void *argument) {
    Worker *worker = argument;
    double drain_deadline = 0;
    for (;;) {
        if (stopping) {
            if (!drain_deadline) {
                drain_deadline = now_seconds() + SHUTDOWN_DRAIN_SECONDS;
                log_event(worker->logger, "worker draining active responses");
            }
            add_pending(worker);
            Connection **position = &worker->clients;
            while (*position) {
                Connection *client = *position;
                if (client->state == READING) {
                    remove_client(worker, position);
                } else {
                    client->keep_alive = false;
                    position = &client->next;
                }
            }
            if (!worker->clients) {
                break;
            }
            if (now_seconds() >= drain_deadline) {
                log_event(worker->logger, "worker drain timeout: closing remaining connections");
                break;
            }
        }
        fd_set readers, writers;
        FD_ZERO(&readers);
        FD_ZERO(&writers);
        FD_SET(worker->wake[0], &readers);
        int max_fd = worker->wake[0];
        // select ждёт чтение запроса либо запись уже подготовленного ответа
        for (Connection *client = worker->clients; client; client = client->next) {
            if (client->state == READING && client->input_len < HEADER_LIMIT) {
                FD_SET(client->fd, &readers);
            }
            if (client->state != READING) {
                FD_SET(client->fd, &writers);
            }
            if (client->fd > max_fd) {
                max_fd = client->fd;
            }
        }
        struct timeval timeout = {1, 0};
        int ready = select(max_fd + 1, &readers, &writers, NULL, &timeout);
        if (ready < 0 && errno != EINTR) {
            log_event(worker->logger, "worker select failed: %s", strerror(errno));
            atomic_store(&stopping, true);
            break;
        }
        add_pending(worker);
        double current = now_seconds();
        Connection **position = &worker->clients;
        while (*position) {
            Connection *client = *position;
            bool alive = true;
            if (stopping && client->state == READING) {
                alive = false;
            }
            if (stopping) {
                client->keep_alive = false;
            }
            if (alive && ready > 0 && client->state == READING && FD_ISSET(client->fd, &readers)) {
                alive = read_socket(worker, client);
            }
            if (alive && ready > 0 && client->state != READING && FD_ISSET(client->fd, &writers)) {
                alive = write_socket(worker, client);
            }
            if (alive && current - client->last_activity > IDLE_SECONDS) {
                alive = false;
            }
            if (!alive) {
                remove_client(worker, position);
            } else {
                position = &client->next;
            }
        }
    }
    add_pending(worker);
    while (worker->clients) {
        remove_client(worker, &worker->clients);
    }
    return NULL;
}

int worker_enqueue(Worker *worker, int fd) {
    Pending *pending = malloc(sizeof(*pending));
    if (!pending) {
        return -1;
    }
    pending->fd = fd;
    pending->next = NULL;
    pthread_mutex_lock(&worker->queue_mutex);
    if (worker->queue_tail) {
        worker->queue_tail->next = pending;
    } else {
        worker->queue_head = pending;
    }
    worker->queue_tail = pending;
    atomic_fetch_add(worker->active, 1);
    atomic_fetch_add(&worker->load, 1);
    pthread_mutex_unlock(&worker->queue_mutex);
    char byte = 1;
    if (write(worker->wake[1], &byte, 1) < 0 && errno != EAGAIN) {
        return 1;
    }
    return 0;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Создаёт worker с собственной очередью соединений и каналом пробуждения
Worker *worker_create(int root_fd, Logger *logger, atomic_int *active) {
    Worker *worker = calloc(1, sizeof(*worker));
    if (!worker) {
        return NULL;
    }
    worker->root_fd = root_fd;
    worker->logger = logger;
    worker->active = active;
    if (pipe(worker->wake) < 0) {
        free(worker);
        return NULL;
    }
    if (worker->wake[0] >= FD_SETSIZE || !set_nonblocking(worker->wake[0]) ||
        !set_nonblocking(worker->wake[1])) {
        close(worker->wake[0]);
        close(worker->wake[1]);
        free(worker);
        return NULL;
    }
    if (pthread_mutex_init(&worker->queue_mutex, NULL) != 0) {
        close(worker->wake[0]);
        close(worker->wake[1]);
        free(worker);
        return NULL;
    }
    if (pthread_create(&worker->thread, NULL, worker_main, worker) != 0) {
        pthread_mutex_destroy(&worker->queue_mutex);
        close(worker->wake[0]);
        close(worker->wake[1]);
        free(worker);
        return NULL;
    }
    return worker;
}

int worker_load(const Worker *worker) {
    return atomic_load(&worker->load);
}

void worker_notify(Worker *worker) {
    char byte = 1;
    (void)write(worker->wake[1], &byte, 1);
}

void worker_join_destroy(Worker *worker) {
    pthread_join(worker->thread, NULL);
    close(worker->wake[0]);
    close(worker->wake[1]);
    pthread_mutex_destroy(&worker->queue_mutex);
    free(worker);
}

