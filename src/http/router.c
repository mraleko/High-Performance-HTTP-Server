#include "http_router.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "metrics.h"
#include "util.h"

static const char *content_type_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (util_ascii_casecmp(dot, ".txt") == 0) {
        return "text/plain";
    }
    if (util_ascii_casecmp(dot, ".html") == 0 || util_ascii_casecmp(dot, ".htm") == 0) {
        return "text/html";
    }
    if (util_ascii_casecmp(dot, ".json") == 0) {
        return "application/json";
    }
    if (util_ascii_casecmp(dot, ".css") == 0) {
        return "text/css";
    }
    if (util_ascii_casecmp(dot, ".js") == 0) {
        return "application/javascript";
    }
    if (util_ascii_casecmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (util_ascii_casecmp(dot, ".jpg") == 0 || util_ascii_casecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    return "application/octet-stream";
}

void http_response_reset(http_response_t *resp) {
    if (resp == NULL) {
        return;
    }

    if (resp->file_fd >= 0) {
        close(resp->file_fd);
    }

    memset(resp, 0, sizeof(*resp));
    resp->file_fd = -1;
}

static int response_prepare_head(
    http_response_t *resp,
    int status,
    const char *reason,
    const char *content_type,
    size_t content_length,
    bool close_after_send
) {
    const char *connection = close_after_send ? "close" : "keep-alive";
    int n = snprintf(
        resp->head,
        sizeof(resp->head),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: %s\r\n"
        "Connection: %s\r\n"
        "\r\n",
        status,
        reason,
        content_length,
        content_type,
        connection
    );
    if (n < 0 || (size_t)n >= sizeof(resp->head)) {
        return -1;
    }

    resp->active = true;
    resp->close_after_send = close_after_send;
    resp->head_len = (size_t)n;
    resp->head_sent = 0;
    resp->body_sent = 0;
    resp->file_offset = 0;
    return 0;
}

static int response_prepare_memory(
    http_response_t *resp,
    int status,
    const char *reason,
    const char *content_type,
    const char *body,
    size_t body_len,
    bool close_after_send
) {
    if (body_len > sizeof(resp->body)) {
        return -1;
    }

    if (response_prepare_head(resp, status, reason, content_type, body_len, close_after_send) != 0) {
        return -1;
    }

    if (body_len > 0) {
        memcpy(resp->body, body, body_len);
    }
    resp->body_len = body_len;
    resp->file_fd = -1;
    resp->file_remaining = 0;
    return 0;
}

static int route_not_found(http_response_t *resp, bool close_after_send) {
    static const char body[] = "not found\n";
    return response_prepare_memory(
        resp,
        404,
        "Not Found",
        "text/plain",
        body,
        sizeof(body) - 1,
        close_after_send
    );
}

static int route_method_not_allowed(http_response_t *resp, bool close_after_send) {
    static const char body[] = "method not allowed\n";
    return response_prepare_memory(
        resp,
        405,
        "Method Not Allowed",
        "text/plain",
        body,
        sizeof(body) - 1,
        close_after_send
    );
}

static int route_bad_request(http_response_t *resp, bool close_after_send) {
    static const char body[] = "bad request\n";
    return response_prepare_memory(
        resp,
        400,
        "Bad Request",
        "text/plain",
        body,
        sizeof(body) - 1,
        close_after_send
    );
}

static int route_payload_too_large(http_response_t *resp, bool close_after_send) {
    static const char body[] = "payload too large\n";
    return response_prepare_memory(
        resp,
        413,
        "Payload Too Large",
        "text/plain",
        body,
        sizeof(body) - 1,
        close_after_send
    );
}

static int route_server_error(http_response_t *resp, bool close_after_send) {
    static const char body[] = "internal server error\n";
    return response_prepare_memory(
        resp,
        500,
        "Internal Server Error",
        "text/plain",
        body,
        sizeof(body) - 1,
        close_after_send
    );
}

int http_build_error_response(http_response_t *resp, int status, bool close_after_send) {
    switch (status) {
        case 400:
            return route_bad_request(resp, close_after_send);
        case 404:
            return route_not_found(resp, close_after_send);
        case 405:
            return route_method_not_allowed(resp, close_after_send);
        case 413:
            return route_payload_too_large(resp, close_after_send);
        case 414: {
            static const char body[] = "uri too long\\n";
            return response_prepare_memory(
                resp,
                414,
                "URI Too Long",
                "text/plain",
                body,
                sizeof(body) - 1,
                close_after_send
            );
        }
        case 431: {
            static const char body[] = "request header fields too large\\n";
            return response_prepare_memory(
                resp,
                431,
                "Request Header Fields Too Large",
                "text/plain",
                body,
                sizeof(body) - 1,
                close_after_send
            );
        }
        case 505: {
            static const char body[] = "http version not supported\\n";
            return response_prepare_memory(
                resp,
                505,
                "HTTP Version Not Supported",
                "text/plain",
                body,
                sizeof(body) - 1,
                close_after_send
            );
        }
        default:
            return route_server_error(resp, close_after_send);
    }
}

int http_route_request(
    const http_request_t *req,
    http_response_t *resp,
    const char *static_root,
    bool force_close
) {
    if (req == NULL || resp == NULL || static_root == NULL) {
        return -1;
    }

    char path[HTTP_MAX_PATH_LEN + 1];
    size_t path_len = strcspn(req->path, "?");
    if (path_len > HTTP_MAX_PATH_LEN) {
        return route_bad_request(resp, true);
    }
    memcpy(path, req->path, path_len);
    path[path_len] = '\0';

    bool close_after_send = force_close || req->connection_close;

    if (strcmp(path, "/healthz") == 0) {
        if (util_ascii_casecmp(req->method, "GET") != 0) {
            return route_method_not_allowed(resp, close_after_send);
        }
        static const char body[] = "ok";
        return response_prepare_memory(
            resp,
            200,
            "OK",
            "text/plain",
            body,
            sizeof(body) - 1,
            close_after_send
        );
    }

    if (strcmp(path, "/metrics") == 0) {
        if (util_ascii_casecmp(req->method, "GET") != 0) {
            return route_method_not_allowed(resp, close_after_send);
        }

        size_t metric_len = 0;
        metrics_render_plain(resp->body, sizeof(resp->body), &metric_len);
        if (response_prepare_head(resp, 200, "OK", "text/plain", metric_len, close_after_send) != 0) {
            return route_server_error(resp, true);
        }
        resp->body_len = metric_len;
        resp->file_fd = -1;
        resp->file_remaining = 0;
        return 0;
    }

    if (strcmp(path, "/echo") == 0) {
        if (util_ascii_casecmp(req->method, "POST") != 0) {
            return route_method_not_allowed(resp, close_after_send);
        }

        if (req->body_len > sizeof(resp->body)) {
            return route_payload_too_large(resp, close_after_send);
        }

        if (response_prepare_head(resp, 200, "OK", "application/octet-stream", req->body_len, close_after_send) != 0) {
            return route_server_error(resp, true);
        }

        if (req->body_len > 0) {
            memcpy(resp->body, req->body, req->body_len);
        }
        resp->body_len = req->body_len;
        resp->file_fd = -1;
        resp->file_remaining = 0;
        return 0;
    }

    if (strncmp(path, "/static/", 8) == 0) {
        if (util_ascii_casecmp(req->method, "GET") != 0) {
            return route_method_not_allowed(resp, close_after_send);
        }

        const char *rel = path + 8;
        if (!util_static_path_is_safe(rel)) {
            return route_bad_request(resp, close_after_send);
        }

        char full_path[2048];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", static_root, rel);
        if (n < 0 || (size_t)n >= sizeof(full_path)) {
            return route_bad_request(resp, close_after_send);
        }

        int fd = open(full_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT || errno == ENOTDIR) {
                return route_not_found(resp, close_after_send);
            }
            return route_server_error(resp, true);
        }

        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            return route_not_found(resp, close_after_send);
        }

        if (st.st_size < 0) {
            close(fd);
            return route_server_error(resp, true);
        }

        if (response_prepare_head(
                resp,
                200,
                "OK",
                content_type_for_path(rel),
                (size_t)st.st_size,
                close_after_send
            ) != 0) {
            close(fd);
            return route_server_error(resp, true);
        }

        resp->body_len = 0;
        resp->file_fd = fd;
        resp->file_offset = 0;
        resp->file_remaining = st.st_size;
        return 0;
    }

    return route_not_found(resp, close_after_send);
}
