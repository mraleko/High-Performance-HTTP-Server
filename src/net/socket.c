#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int net_set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

int net_create_listener(int port, int backlog, int reuse_port) {
    int sock_type = SOCK_STREAM;
#ifdef SOCK_NONBLOCK
    sock_type |= SOCK_NONBLOCK;
#endif
#ifdef SOCK_CLOEXEC
    sock_type |= SOCK_CLOEXEC;
#endif

    int fd = socket(AF_INET, sock_type, 0);
    if (fd < 0) {
        return -1;
    }

#ifndef SOCK_NONBLOCK
    if (net_set_nonblocking(fd) != 0) {
        close(fd);
        return -1;
    }
#endif

#ifndef SOCK_CLOEXEC
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags < 0 || fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) != 0) {
        close(fd);
        return -1;
    }
#endif

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        close(fd);
        return -1;
    }

#ifdef SO_REUSEPORT
    if (reuse_port) {
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
            close(fd);
            return -1;
        }
    }
#else
    (void)reuse_port;
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}
