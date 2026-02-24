#include "util.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

uint64_t util_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

int util_ascii_casecmp(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return ca - cb;
        }
        ++a;
        ++b;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int util_ascii_ncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (a[i] == '\0' || b[i] == '\0') {
            return tolower((unsigned char)a[i]) - tolower((unsigned char)b[i]);
        }
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) {
            return ca - cb;
        }
    }
    return 0;
}

const char *util_trim_left(const char *s) {
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    return s;
}

void util_trim_right(char *s) {
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c != ' ' && c != '\t') {
            break;
        }
        s[len - 1] = '\0';
        --len;
    }
}

bool util_static_path_is_safe(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    if (path[0] == '/') {
        return false;
    }

    const char *seg = path;
    for (const char *p = path;; ++p) {
        if (*p == '\\') {
            return false;
        }
        if (*p == '/' || *p == '\0') {
            size_t seg_len = (size_t)(p - seg);
            if (seg_len == 0) {
                return false;
            }
            if (seg_len == 1 && seg[0] == '.') {
                return false;
            }
            if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
                return false;
            }
            if (*p == '\0') {
                break;
            }
            seg = p + 1;
        }
    }

    return true;
}
