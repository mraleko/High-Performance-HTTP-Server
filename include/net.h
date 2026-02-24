#ifndef NET_H
#define NET_H

int net_set_nonblocking(int fd);
int net_create_listener(int port, int backlog, int reuse_port);

#endif
