#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLI_MAX_OPTIONS 5

static int parse_number(const char *text, int minimum, int maximum) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || !text[0] || *end || value < minimum || value > maximum) {
        return -1;
    }
    return (int)value;
}

static void print_usage(void) {
    puts("Usage: static-server --root DIR [--host IPv4] [--port 1..65535] [--threads 1..32] [--log FILE]");
}

static int parse_options(int argc, char **argv,
                         ServerOption options[CLI_MAX_OPTIONS], size_t *count) {
    *count = 0;
    int root_set = 0;
    for (int i = 1; i < argc; i += 2) {
        if (!strcmp(argv[i], "--help") && i == argc - 1) {
            return 1;
        }
        if (i + 1 >= argc) {
            return -1;
        }
        const char *value = argv[i + 1];
        ServerOption option;
        if (!strcmp(argv[i], "--host")) {
            option = server_option_host(value);
        } else if (!strcmp(argv[i], "--root")) {
            option = server_option_root(value);
            root_set = 1;
        } else if (!strcmp(argv[i], "--log")) {
            option = server_option_log(value);
        } else if (!strcmp(argv[i], "--port")) {
            int port = parse_number(value, 1, 65535);
            if (port < 0) {
                return -1;
            }
            option = server_option_port(port);
        } else if (!strcmp(argv[i], "--threads")) {
            int threads = parse_number(value, 1, 32);
            if (threads < 0) {
                return -1;
            }
            option = server_option_threads(threads);
        } else {
            return -1;
        }
        size_t index = 0;
        while (index < *count && options[index].apply != option.apply) {
            ++index;
        }
        if (index == *count) {
            if (*count == CLI_MAX_OPTIONS) {
                return -1;
            }
            ++*count;
         }
        options[index] = option;
    }
    return root_set ? 0 : -1;
}

int main(int argc, char **argv) {
    ServerOption options[CLI_MAX_OPTIONS];
    size_t count = 0;
    int parsed = parse_options(argc, argv, options, &count);
    if (parsed == 1) {
        print_usage();
        return 0;
    }
    if (parsed < 0) {
        print_usage();
        return 1;
    }

    Server *server = server_create(options, count);
    if (!server) {
        fprintf(stderr, "invalid server options\n");
        return 1;
    }
    int result = server_run(server);
    server_destroy(server);
    return result;
}
