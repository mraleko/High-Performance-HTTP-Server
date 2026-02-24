#include "server.h"

#ifdef __linux__

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http_parser.h"
#include "http_router.h"
#include "metrics.h"
#include "net.h"
#include "util.h"

#define MAX_EVENTS 256

static volatile sig_atomic_t g_stop = 0;

typedef struct {
    int id;
    server_config_t cfg;
    int epoll_fd;
    int listen_fd;
    connection_t **conns;
    size_t conns_cap;
} worker_ctx_t;

static void on_signal(int signo) {
    (void)signo;
    g_stop = 1;
}

static int install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;

    if (sigaction(SIGINT, &sa, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        return -1;
    }
    return 0;
}

static int ensure_conn_capacity(worker_ctx_t *ctx, int fd) {
    if (fd < 0) {
        return -1;
    }
    if ((size_t)fd < ctx->conns_cap) {
        return 0;
    }

    size_t new_cap = ctx->conns_cap == 0 ? 1024 : ctx->conns_cap;
    while (new_cap <= (size_t)fd) {
        if (new_cap > SIZE_MAX / 2) {
            return -1;
        }
        new_cap *= 2;
    }

    connection_t **next = realloc(ctx->conns, new_cap * sizeof(*ctx->conns));
    if (next == NULL) {
        return -1;
    }

    memset(next + ctx->conns_cap, 0, (new_cap - ctx->conns_cap) * sizeof(*ctx->conns));
    ctx->conns = next;
    ctx->conns_cap = new_cap;
    return 0;
}

static connection_t *conn_create(int fd) {
    connection_t *conn = calloc(1, sizeof(*conn));
    if (conn == NULL) {
        return NULL;
    }
    conn->fd = fd;
    conn->last_active_ms = util_now_ms();
    conn->resp.file_fd = -1;
    return conn;
}

static void conn_free(connection_t *conn) {
    if (conn == NULL) {
        return;
    }
    http_response_reset(&conn->resp);
    free(conn);
}

static void close_connection(worker_ctx_t *ctx, int fd) {
    if (fd < 0 || (size_t)fd >= ctx->conns_cap) {
        return;
    }
    connection_t *conn = ctx->conns[fd];
    if (conn == NULL) {
        return;
    }

    epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    ctx->conns[fd] = NULL;
    metrics_dec_connections();
    conn_free(conn);
}

static int update_conn_interest(worker_ctx_t *ctx, connection_t *conn) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = conn->fd;
    ev.events = EPOLLIN | EPOLLET;
    if (conn->resp.active) {
        ev.events |= EPOLLOUT;
    }

    return epoll_ctl(ctx->epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
}

static void compact_input_buffer(connection_t *conn, size_t consumed) {
    if (consumed >= conn->in_len) {
        conn->in_len = 0;
        return;
    }

    size_t remaining = conn->in_len - consumed;
    memmove(conn->in_buf, conn->in_buf + consumed, remaining);
    conn->in_len = remaining;
}

static void prepare_parse_error_response(connection_t *conn, int status) {
    http_response_reset(&conn->resp);
    if (http_build_error_response(&conn->resp, status, true) != 0) {
        http_response_reset(&conn->resp);
        (void)http_build_error_response(&conn->resp, 500, true);
    }
    conn->in_len = 0;
}

static void try_parse_and_route(worker_ctx_t *ctx, connection_t *conn) {
    while (!conn->resp.active) {
        if (conn->in_len == 0) {
            return;
        }

        http_request_t req;
        size_t consumed = 0;
        int error_status = 400;
        http_parse_result_t res = http_parse_request(
            conn->in_buf,
            conn->in_len,
            &req,
            &consumed,
            &error_status
        );

        if (res == HTTP_PARSE_INCOMPLETE) {
            return;
        }

        metrics_inc_requests();

        if (res == HTTP_PARSE_ERROR) {
            prepare_parse_error_response(conn, error_status);
            return;
        }

        http_response_reset(&conn->resp);
        if (http_route_request(&req, &conn->resp, ctx->cfg.static_root, false) != 0) {
            http_response_reset(&conn->resp);
            (void)http_build_error_response(&conn->resp, 500, true);
        }

        compact_input_buffer(conn, consumed);
        return;
    }
}

static int flush_response(worker_ctx_t *ctx, int fd) {
    if ((size_t)fd >= ctx->conns_cap) {
        return -1;
    }

    connection_t *conn = ctx->conns[fd];
    if (conn == NULL) {
        return -1;
    }

    while (true) {
        if (!conn->resp.active) {
            try_parse_and_route(ctx, conn);
            if (!conn->resp.active) {
                break;
            }
        }

        while (conn->resp.head_sent < conn->resp.head_len) {
            ssize_t n = write(
                fd,
                conn->resp.head + conn->resp.head_sent,
                conn->resp.head_len - conn->resp.head_sent
            );
            if (n > 0) {
                conn->resp.head_sent += (size_t)n;
                metrics_add_bytes_out((size_t)n);
                conn->last_active_ms = util_now_ms();
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return 0;
            }
            close_connection(ctx, fd);
            return -1;
        }

        while (conn->resp.body_sent < conn->resp.body_len) {
            ssize_t n = write(
                fd,
                conn->resp.body + conn->resp.body_sent,
                conn->resp.body_len - conn->resp.body_sent
            );
            if (n > 0) {
                conn->resp.body_sent += (size_t)n;
                metrics_add_bytes_out((size_t)n);
                conn->last_active_ms = util_now_ms();
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return 0;
            }
            close_connection(ctx, fd);
            return -1;
        }

        while (conn->resp.file_fd >= 0 && conn->resp.file_remaining > 0) {
            off_t off = conn->resp.file_offset;
            ssize_t n = sendfile(fd, conn->resp.file_fd, &off, (size_t)conn->resp.file_remaining);
            if (n > 0) {
                conn->resp.file_offset = off;
                conn->resp.file_remaining -= n;
                metrics_add_bytes_out((size_t)n);
                conn->last_active_ms = util_now_ms();
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return 0;
            }
            close_connection(ctx, fd);
            return -1;
        }

        bool close_after = conn->resp.close_after_send;
        http_response_reset(&conn->resp);
        if (close_after) {
            close_connection(ctx, fd);
            return -1;
        }

        if (conn->in_len == 0) {
            break;
        }
    }

    if ((size_t)fd < ctx->conns_cap && ctx->conns[fd] != NULL) {
        if (update_conn_interest(ctx, ctx->conns[fd]) != 0) {
            close_connection(ctx, fd);
            return -1;
        }
    }

    return 0;
}

static void handle_client_read(worker_ctx_t *ctx, int fd) {
    if ((size_t)fd >= ctx->conns_cap || ctx->conns[fd] == NULL) {
        return;
    }

    connection_t *conn = ctx->conns[fd];
    char overflow_buf[4096];

    for (;;) {
        ssize_t n;
        if (conn->in_len < sizeof(conn->in_buf)) {
            n = read(fd, conn->in_buf + conn->in_len, sizeof(conn->in_buf) - conn->in_len);
        } else {
            n = read(fd, overflow_buf, sizeof(overflow_buf));
        }

        if (n > 0) {
            metrics_add_bytes_in((size_t)n);
            conn->last_active_ms = util_now_ms();

            if (conn->in_len < sizeof(conn->in_buf)) {
                conn->in_len += (size_t)n;
            } else if (!conn->resp.active) {
                prepare_parse_error_response(conn, 413);
            }
            continue;
        }

        if (n == 0) {
            close_connection(ctx, fd);
            return;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        close_connection(ctx, fd);
        return;
    }

    conn = ((size_t)fd < ctx->conns_cap) ? ctx->conns[fd] : NULL;
    if (conn == NULL) {
        return;
    }

    try_parse_and_route(ctx, conn);

    if ((size_t)fd < ctx->conns_cap && ctx->conns[fd] != NULL) {
        if (flush_response(ctx, fd) != 0) {
            return;
        }
    }

    if ((size_t)fd < ctx->conns_cap && ctx->conns[fd] != NULL) {
        if (update_conn_interest(ctx, ctx->conns[fd]) != 0) {
            close_connection(ctx, fd);
        }
    }
}

static void handle_accept(worker_ctx_t *ctx) {
    for (;;) {
        int client_fd = accept4(ctx->listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            return;
        }

        int one = 1;
        (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if (ensure_conn_capacity(ctx, client_fd) != 0) {
            close(client_fd);
            continue;
        }

        connection_t *conn = conn_create(client_fd);
        if (conn == NULL) {
            close(client_fd);
            continue;
        }

        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.data.fd = client_fd;
        ev.events = EPOLLIN | EPOLLET;

        if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) != 0) {
            conn_free(conn);
            close(client_fd);
            continue;
        }

        ctx->conns[client_fd] = conn;
        metrics_inc_connections();
    }
}

static void close_idle_connections(worker_ctx_t *ctx, uint64_t now_ms) {
    uint64_t timeout_ms = (uint64_t)ctx->cfg.idle_timeout_sec * 1000ULL;
    for (size_t i = 0; i < ctx->conns_cap; ++i) {
        connection_t *conn = ctx->conns[i];
        if (conn == NULL) {
            continue;
        }
        if (now_ms - conn->last_active_ms > timeout_ms) {
            close_connection(ctx, conn->fd);
        }
    }
}

static int worker_init(worker_ctx_t *ctx) {
    ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }

    ctx->listen_fd = net_create_listener(ctx->cfg.port, ctx->cfg.backlog, 1);
    if (ctx->listen_fd < 0) {
        perror("net_create_listener");
        close(ctx->epoll_fd);
        ctx->epoll_fd = -1;
        return -1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = ctx->listen_fd;
    ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev) != 0) {
        perror("epoll_ctl listen add");
        close(ctx->listen_fd);
        close(ctx->epoll_fd);
        ctx->listen_fd = -1;
        ctx->epoll_fd = -1;
        return -1;
    }

    ctx->conns_cap = 1024;
    ctx->conns = calloc(ctx->conns_cap, sizeof(*ctx->conns));
    if (ctx->conns == NULL) {
        fprintf(stderr, "calloc connection table failed\\n");
        close(ctx->listen_fd);
        close(ctx->epoll_fd);
        ctx->listen_fd = -1;
        ctx->epoll_fd = -1;
        return -1;
    }

    return 0;
}

static void worker_destroy(worker_ctx_t *ctx) {
    if (ctx->conns != NULL) {
        for (size_t i = 0; i < ctx->conns_cap; ++i) {
            if (ctx->conns[i] != NULL) {
                close(ctx->conns[i]->fd);
                metrics_dec_connections();
                conn_free(ctx->conns[i]);
                ctx->conns[i] = NULL;
            }
        }
        free(ctx->conns);
        ctx->conns = NULL;
    }

    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
        ctx->epoll_fd = -1;
    }
}

static void *worker_main(void *arg) {
    worker_ctx_t *ctx = arg;

    if (worker_init(ctx) != 0) {
        fprintf(stderr, "worker %d init failed\\n", ctx->id);
        g_stop = 1;
        return NULL;
    }

    struct epoll_event events[MAX_EVENTS];
    uint64_t last_idle_scan_ms = util_now_ms();

    while (!g_stop) {
        int n = epoll_wait(ctx->epoll_fd, events, MAX_EVENTS, 250);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            g_stop = 1;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == ctx->listen_fd) {
                if (ev & EPOLLIN) {
                    handle_accept(ctx);
                }
                continue;
            }

            if ((size_t)fd >= ctx->conns_cap || ctx->conns[fd] == NULL) {
                continue;
            }

            if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                close_connection(ctx, fd);
                continue;
            }

            if (ev & EPOLLIN) {
                handle_client_read(ctx, fd);
            }

            if ((size_t)fd < ctx->conns_cap && ctx->conns[fd] != NULL && (ev & EPOLLOUT)) {
                (void)flush_response(ctx, fd);
            }
        }

        uint64_t now_ms = util_now_ms();
        if (now_ms - last_idle_scan_ms >= 1000) {
            close_idle_connections(ctx, now_ms);
            last_idle_scan_ms = now_ms;
        }
    }

    worker_destroy(ctx);
    return NULL;
}

int server_run(const server_config_t *cfg) {
    if (cfg == NULL || cfg->threads <= 0) {
        return 1;
    }

    if (install_signal_handlers() != 0) {
        perror("sigaction");
        return 1;
    }

    metrics_init();

    pthread_t *threads = calloc((size_t)cfg->threads, sizeof(*threads));
    worker_ctx_t *ctxs = calloc((size_t)cfg->threads, sizeof(*ctxs));
    if (threads == NULL || ctxs == NULL) {
        free(threads);
        free(ctxs);
        return 1;
    }

    for (int i = 0; i < cfg->threads; ++i) {
        ctxs[i].id = i;
        ctxs[i].cfg = *cfg;
        ctxs[i].epoll_fd = -1;
        ctxs[i].listen_fd = -1;

        if (pthread_create(&threads[i], NULL, worker_main, &ctxs[i]) != 0) {
            g_stop = 1;
            for (int j = 0; j < i; ++j) {
                pthread_join(threads[j], NULL);
            }
            free(threads);
            free(ctxs);
            return 1;
        }
    }

    fprintf(
        stderr,
        "httpd listening on 0.0.0.0:%d with %d thread(s), static_root=%s, idle_timeout=%ds\n",
        cfg->port,
        cfg->threads,
        cfg->static_root,
        cfg->idle_timeout_sec
    );

    for (int i = 0; i < cfg->threads; ++i) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    free(ctxs);
    return 0;
}

#else

#include <stdio.h>

int server_run(const server_config_t *cfg) {
    (void)cfg;
    fprintf(stderr, "httpd requires Linux (epoll/sendfile/accept4).\\n");
    return 1;
}

#endif
