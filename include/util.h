#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint64_t util_now_ms(void);
int util_ascii_casecmp(const char *a, const char *b);
int util_ascii_ncasecmp(const char *a, const char *b, size_t n);
const char *util_trim_left(const char *s);
void util_trim_right(char *s);
bool util_static_path_is_safe(const char *path);

#endif
