#ifndef STATIC_SERVER_SERVER_H
#define STATIC_SERVER_SERVER_H

#include <stddef.h>

typedef struct Server {
    char *host;
    char *root;
    char *log;
    int port;
    int threads;
} Server;
typedef struct ServerOption ServerOption;
typedef int (*ServerOptionApply)(Server *server, const ServerOption *option);

struct ServerOption {
    ServerOptionApply apply;
    union {
        const char *text;
        int number;
    } value;
};

ServerOption server_option_host(const char *host);
ServerOption server_option_port(int port);
ServerOption server_option_root(const char *root);
ServerOption server_option_threads(int threads);
ServerOption server_option_log(const char *path);

Server *server_create(const ServerOption *options, size_t count);
int server_run(Server *server);
void server_destroy(Server *server);

#endif
