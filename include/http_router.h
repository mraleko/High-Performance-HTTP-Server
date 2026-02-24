#ifndef HTTP_ROUTER_H
#define HTTP_ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "http_parser.h"

#define HTTP_RESPONSE_HEAD_CAP 2048
#define HTTP_RESPONSE_BODY_CAP (128 * 1024)

typedef struct {
    bool active;
    bool close_after_send;

    char head[HTTP_RESPONSE_HEAD_CAP];
    size_t head_len;
    size_t head_sent;

    char body[HTTP_RESPONSE_BODY_CAP];
    size_t body_len;
    size_t body_sent;

    int file_fd;
    off_t file_offset;
    off_t file_remaining;
} http_response_t;

void http_response_reset(http_response_t *resp);
int http_route_request(
    const http_request_t *req,
    http_response_t *resp,
    const char *static_root,
    bool force_close
);
int http_build_error_response(http_response_t *resp, int status, bool close_after_send);

#endif
