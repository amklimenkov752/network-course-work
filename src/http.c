#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *reason(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 414:
            return "URI Too Long";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        default:
            return "Error";
    }
}

static const MimeType mime_types[] = {
    {".html", "text/html; charset=utf-8"},
    {".htm", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".txt", "text/plain; charset=utf-8"},
    {".json", "application/json"},
    {".svg", "image/svg+xml"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".webp", "image/webp"},
    {".ico", "image/x-icon"},
    {".pdf", "application/pdf"}
};

static const MimeType fallback_mime_type = {NULL, "application/octet-stream"};

const MimeType *mime_type(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) {
        return &fallback_mime_type;
    }
    for (size_t i = 0; i < sizeof(mime_types) / sizeof(mime_types[0]); ++i) {
        if (!strcasecmp(ext, mime_types[i].extension)) {
            return &mime_types[i];
        }
    }
    return &fallback_mime_type;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Процентное кодирование раскрывается до проверки компонентов пути
int decode_path(const char *target, char path[TARGET_LIMIT + 1]) {
    size_t length = strlen(target);
    if (length > TARGET_LIMIT) {
        return 414;
    }
    if (!length || target[0] != '/') {
        return 400;
    }
    size_t used = 0;
    for (size_t i = 0; i < length && target[i] != '?'; ++i) {
        unsigned char c = (unsigned char)target[i];
        if (c == '%') {
            if (i + 2 >= length) {
                return 400;
            }
            int hi = hex_digit(target[i + 1]);
            int lo = hex_digit(target[i + 2]);
            if (hi < 0 || lo < 0) {
                return 400;
            }
            c = (unsigned char)((hi << 4) | lo);
            i += 2;
            if (c == '/' || c == '\\') {
                return 403;
            }
        }
        if (c < 32 || c == 127 || c == '\\' || c == '#') {
            return 400;
        }
        path[used++] = (char)c;
    }
    path[used] = '\0';
    return 200;
}

// Открывает путь по компонентам, не позволяя выйти из корневого каталога
int open_static(int root_fd, char *path, uint64_t *size, char **name) {
    int current = dup(root_fd);
    if (current < 0) {
        return -500;
    }
    bool trailing_slash = path[strlen(path) - 1] == '/';
    char *save = NULL;
    char *part = strtok_r(path, "/", &save);
    // Каждый компонент открывается относительно уже проверенного каталога
    while (part) {
        if (part[0] == '.') {
            close(current);
            return -403;
        }
        char *next_part = strtok_r(NULL, "/", &save);
        bool last = !next_part && !trailing_slash;
        int next = openat(current, part, O_RDONLY | O_NONBLOCK | O_NOFOLLOW |
                                           (last ? 0 : O_DIRECTORY));
        int error = errno;
        close(current);
        if (next < 0) {
            if (error == ENOENT) {
                return -404;
            }
            if (error == ELOOP || error == EACCES || error == EPERM ||
                error == ENOTDIR || error == ENXIO || error == ENODEV) {
                return -403;
            }
            return -500;
        }
        current = next;
        if (last) {
            *name = part;
        }
        part = next_part;
    }
    if (trailing_slash) {
        int next = openat(current, "index.html", O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
        int error = errno;
        close(current);
        if (next < 0) {
            if (error == ENOENT) {
                return -404;
            }
            if (error == ELOOP || error == EACCES || error == EPERM ||
                error == ENOTDIR || error == ENXIO || error == ENODEV) {
                return -403;
            }
            return -500;
        }
        current = next;
        *name = "index.html";
    }
    struct stat info;
    if (fstat(current, &info) < 0) {
        close(current);
        return -500;
    }
    if (!S_ISREG(info.st_mode)) {
        close(current);
        return -403;
    }
    if (info.st_size < 0) {
        close(current);
        return -500;
    }
    *size = (uint64_t)info.st_size;
    return current;
}

