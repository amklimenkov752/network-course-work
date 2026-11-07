#include "server.h"
#include "worker.h"
#include "logger.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CONNECTIONS 400
#define MAX_WORKERS 32

_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "signal-safe shutdown requires lock-free atomic_bool");
atomic_bool stopping = false;

static void on_signal(int number) {
    (void)number;
    atomic_store(&stopping, true);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int replace_text(char **destination, const char *value) {
    if (!value || !*value) {
        errno = EINVAL;
        return -1;
    }
    char *copy = strdup(value);
    if (!copy) {
        return -1;
    }
    free(*destination);
    *destination = copy;
    return 0;
}

static int apply_host(Server *server, const ServerOption *option) {
    return replace_text(&server->host, option->value.text);
}

static int apply_root(Server *server, const ServerOption *option) {
    return replace_text(&server->root, option->value.text);
}

static int apply_log(Server *server, const ServerOption *option) {
    return replace_text(&server->log, option->value.text);
}

static int apply_port(Server *server, const ServerOption *option) {
    if (option->value.number < 1 || option->value.number > 65535) {
        errno = EINVAL;
        return -1;
    }
    server->port = option->value.number;
    return 0;
}

static int apply_threads(Server *server, const ServerOption *option) {
    if (option->value.number < 1 || option->value.number > MAX_WORKERS) {
        errno = EINVAL;
        return -1;
    }
    server->threads = option->value.number;
    return 0;
}

ServerOption server_option_host(const char *host) {
    return (ServerOption){.apply = apply_host, .value.text = host};
}

ServerOption server_option_port(int port) {
    return (ServerOption){.apply = apply_port, .value.number = port};
}

ServerOption server_option_root(const char *root) {
    return (ServerOption){.apply = apply_root, .value.text = root};
}

ServerOption server_option_threads(int threads) {
    return (ServerOption){.apply = apply_threads, .value.number = threads};
}

ServerOption server_option_log(const char *path) {
    return (ServerOption){.apply = apply_log, .value.text = path};
}

void server_destroy(Server *server) {
    if (!server) {
        return;
    }
    free(server->host);
    free(server->root);
    free(server->log);
    free(server);
}

Server *server_create(const ServerOption *options, size_t count) {
    if (count && !options) {
        errno = EINVAL;
        return NULL;
    }
    Server *server = calloc(1, sizeof(*server));
    if (!server) {
        return NULL;
    }
    server->host = strdup("127.0.0.1");
    server->log = strdup("logs/server.log");
    server->port = 8080;
    server->threads = 4;
    if (!server->host || !server->log) {
        server_destroy(server);
        return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
        const ServerOption *option = &options[i];
        if (!option->apply) {
            errno = EINVAL;
            server_destroy(server);
            return NULL;
        }
        if (option->apply(server, option) < 0) {
            server_destroy(server);
            return NULL;
        }
    }
    if (!server->root) {
        errno = EINVAL;
        server_destroy(server);
        return NULL;
    }
    struct in_addr address;
    if (inet_pton(AF_INET, server->host, &address) != 1) {
        errno = EINVAL;
        server_destroy(server);
        return NULL;
    }
    return server;
}

// Управляет listener, распределяет соединения и координирует завершение потоков
int server_run(Server *server) {
    if (!server) {
        return 1;
    }
    atomic_store(&stopping, false);
    char *root = realpath(server->root, NULL);
    if (!root) {
        perror("static root");
        return 1;
    }
    int root_fd = open(root, O_RDONLY | O_DIRECTORY);
    if (root_fd < 0) {
        perror("static root");
        free(root);
        return 1;
    }
    Logger log = {0};
    if (logger_open(&log, server->log, root) < 0) {
        fprintf(stderr, "log must be writable and outside the static root\n");
        close(root_fd);
        free(root);
        return 1;
    }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        perror("socket");
        logger_close(&log);
        close(root_fd);
        free(root);
        return 1;
    }
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)server->port);
    if (inet_pton(AF_INET, server->host, &address.sin_addr) != 1 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 256) < 0 || !set_nonblocking(listener) || listener >= FD_SETSIZE) {
        perror("bind/listen");
        close(listener);
        logger_close(&log);
        close(root_fd);
        free(root);
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    atomic_int active = 0;
    bool failed = false;
    Worker *workers[MAX_WORKERS] = {0};
    int started = 0;
    for (int i = 0; i < server->threads; ++i) {
        workers[i] = worker_create(root_fd, &log, &active);
        if (!workers[i]) {
            fprintf(stderr, "cannot start worker\n");
            atomic_store(&stopping, true);
            failed = true;
            break;
        }
        ++started;
    }
    if (!stopping) {
        log_event(&log, "listening host=%s port=%d root=%s threads=%d",
                  server->host, server->port, root, started);
        printf("Listening on http://%s:%d (root: %s, pid: %ld)\n",
               server->host, server->port, root, (long)getpid());
        fflush(stdout);
    }
    while (!stopping) {
        fd_set readers;
        FD_ZERO(&readers);
        FD_SET(listener, &readers);
        struct timeval timeout = {1, 0};
        int ready = select(listener + 1, &readers, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_event(&log, "accept select failed: %s", strerror(errno));
            failed = true;
            break;
        }
        if (!ready) {
            continue;
        }
        while (!stopping) {
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == ECONNABORTED) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                int error = errno;
                log_event(&log, "accept failed: %s", strerror(error));
                if (error == EMFILE || error == ENFILE || error == ENOBUFS || error == ENOMEM) {
                    struct timespec pause = {1, 0};
                    nanosleep(&pause, NULL);
                } else {
                    failed = true;
                    atomic_store(&stopping, true);
                }
                break;
            }
            if (stopping) {
                close(fd);
                break;
            }
            if (fd >= FD_SETSIZE || atomic_load(&active) >= MAX_CONNECTIONS || !set_nonblocking(fd)) {
                close(fd);
                log_event(&log, "connection rejected: capacity or descriptor limit");
                continue;
            }
            int chosen = 0;
            for (int i = 1; i < started; ++i) {
                if (worker_load(workers[i]) < worker_load(workers[chosen])) {
                    chosen = i;
                }
            }
            // После постановки в очередь клиентский FD принадлежит рабочему потоку
            int queued = worker_enqueue(workers[chosen], fd);
            if (queued < 0) {
                close(fd);
                log_event(&log, "worker queue allocation failed");
            } else if (queued > 0) {
                log_event(&log, "worker queue notification failed");
            }
        }
    }
    atomic_store(&stopping, true);
    log_event(&log, "shutdown requested: stopping accepts and draining responses");
    close(listener);
    for (int i = 0; i < started; ++i) {
        worker_notify(workers[i]);
    }
    for (int i = 0; i < started; ++i) {
        worker_join_destroy(workers[i]);
    }
    log_event(&log, "server stopped");
    logger_close(&log);
    close(root_fd);
    free(root);
    return failed ? 1 : 0;
}
