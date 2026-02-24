#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>

void metrics_init(void);
void metrics_inc_requests(void);
void metrics_add_bytes_in(size_t n);
void metrics_add_bytes_out(size_t n);
void metrics_inc_connections(void);
void metrics_dec_connections(void);
unsigned long long metrics_requests_total(void);
unsigned long long metrics_connections_current(void);
unsigned long long metrics_bytes_in(void);
unsigned long long metrics_bytes_out(void);
double metrics_requests_per_sec(void);
void metrics_render_plain(char *buf, size_t cap, size_t *out_len);

#endif
