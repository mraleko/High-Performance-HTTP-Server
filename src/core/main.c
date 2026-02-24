#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    fprintf(
        stderr,
        "Usage: %s [-p port] [-t threads] [-s static_root] [-i idle_timeout_sec]\n",
        prog
    );
}

static int parse_int_arg(const char *arg, int min, int max, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0') {
        return -1;
    }
    if (v < min || v > max) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv) {
    server_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 8080;
    cfg.threads = 1;
    cfg.backlog = 1024;
    cfg.idle_timeout_sec = 10;
    snprintf(cfg.static_root, sizeof(cfg.static_root), "%s", "./static");

    int opt;
    while ((opt = getopt(argc, argv, "p:t:s:i:h")) != -1) {
        switch (opt) {
            case 'p':
                if (parse_int_arg(optarg, 1, 65535, &cfg.port) != 0) {
                    fprintf(stderr, "invalid port: %s\n", optarg);
                    return 1;
                }
                break;
            case 't':
                if (parse_int_arg(optarg, 1, 128, &cfg.threads) != 0) {
                    fprintf(stderr, "invalid thread count: %s\n", optarg);
                    return 1;
                }
                break;
            case 's':
                if (strlen(optarg) >= sizeof(cfg.static_root)) {
                    fprintf(stderr, "static root path too long\n");
                    return 1;
                }
                snprintf(cfg.static_root, sizeof(cfg.static_root), "%s", optarg);
                break;
            case 'i':
                if (parse_int_arg(optarg, 1, 3600, &cfg.idle_timeout_sec) != 0) {
                    fprintf(stderr, "invalid idle timeout: %s\n", optarg);
                    return 1;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    return server_run(&cfg);
}
