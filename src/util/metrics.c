#include "metrics.h"

#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    atomic_ullong requests_total;
    atomic_ullong connections_current;
    atomic_ullong bytes_in;
    atomic_ullong bytes_out;
    atomic_ullong start_ms;
} metrics_state_t;

static metrics_state_t g_metrics;

static unsigned long long now_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)(ts.tv_nsec / 1000000ULL);
}

void metrics_init(void) {
    atomic_store_explicit(&g_metrics.requests_total, 0, memory_order_relaxed);
    atomic_store_explicit(&g_metrics.connections_current, 0, memory_order_relaxed);
    atomic_store_explicit(&g_metrics.bytes_in, 0, memory_order_relaxed);
    atomic_store_explicit(&g_metrics.bytes_out, 0, memory_order_relaxed);
    atomic_store_explicit(&g_metrics.start_ms, now_monotonic_ms(), memory_order_relaxed);
}

void metrics_inc_requests(void) {
    atomic_fetch_add_explicit(&g_metrics.requests_total, 1, memory_order_relaxed);
}

void metrics_add_bytes_in(size_t n) {
    atomic_fetch_add_explicit(&g_metrics.bytes_in, (unsigned long long)n, memory_order_relaxed);
}

void metrics_add_bytes_out(size_t n) {
    atomic_fetch_add_explicit(&g_metrics.bytes_out, (unsigned long long)n, memory_order_relaxed);
}

void metrics_inc_connections(void) {
    atomic_fetch_add_explicit(&g_metrics.connections_current, 1, memory_order_relaxed);
}

void metrics_dec_connections(void) {
    atomic_fetch_sub_explicit(&g_metrics.connections_current, 1, memory_order_relaxed);
}

unsigned long long metrics_requests_total(void) {
    return atomic_load_explicit(&g_metrics.requests_total, memory_order_relaxed);
}

unsigned long long metrics_connections_current(void) {
    return atomic_load_explicit(&g_metrics.connections_current, memory_order_relaxed);
}

unsigned long long metrics_bytes_in(void) {
    return atomic_load_explicit(&g_metrics.bytes_in, memory_order_relaxed);
}

unsigned long long metrics_bytes_out(void) {
    return atomic_load_explicit(&g_metrics.bytes_out, memory_order_relaxed);
}

double metrics_requests_per_sec(void) {
    unsigned long long start_ms = atomic_load_explicit(&g_metrics.start_ms, memory_order_relaxed);
    unsigned long long now_ms = now_monotonic_ms();
    if (now_ms <= start_ms) {
        return 0.0;
    }
    double elapsed_sec = (double)(now_ms - start_ms) / 1000.0;
    if (elapsed_sec <= 0.0) {
        return 0.0;
    }
    return (double)metrics_requests_total() / elapsed_sec;
}

void metrics_render_plain(char *buf, size_t cap, size_t *out_len) {
    int n = snprintf(
        buf,
        cap,
        "requests_total %llu\n"
        "requests_per_sec %.2f\n"
        "connections_current %llu\n"
        "bytes_in %llu\n"
        "bytes_out %llu\n",
        metrics_requests_total(),
        metrics_requests_per_sec(),
        metrics_connections_current(),
        metrics_bytes_in(),
        metrics_bytes_out()
    );

    if (n < 0) {
        if (out_len != NULL) {
            *out_len = 0;
        }
        return;
    }

    if (out_len != NULL) {
        size_t written = (size_t)n;
        if (written >= cap) {
            written = (cap == 0) ? 0 : cap - 1;
        }
        *out_len = written;
    }
}
