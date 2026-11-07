#ifndef STATIC_SERVER_HTTP_H
#define STATIC_SERVER_HTTP_H

#include <stdint.h>

#define TARGET_LIMIT 4096

typedef struct {
    const char *extension;
    const char *content_type;
} MimeType;

const char *reason(int status);
const MimeType *mime_type(const char *name);
int decode_path(const char *target, char path[TARGET_LIMIT + 1]);
int open_static(int root_fd, char *path, uint64_t *size, char **name);

#endif
