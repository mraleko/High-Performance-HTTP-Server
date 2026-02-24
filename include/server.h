#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "http_router.h"

#define CONN_INBUF_CAP (256 * 1024)

typedef struct connection {
    int fd;
    char in_buf[CONN_INBUF_CAP];
    size_t in_len;
    uint64_t last_active_ms;
    http_response_t resp;
} connection_t;

typedef struct {
    int port;
    int threads;
    int backlog;
    int idle_timeout_sec;
    char static_root[1024];
} server_config_t;

int server_run(const server_config_t *cfg);

#endif
