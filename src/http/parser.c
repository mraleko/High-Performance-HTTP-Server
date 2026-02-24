#include "http_parser.h"

#include <ctype.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util.h"

static ssize_t find_header_end(const char *buf, size_t len) {
    if (len < 4) {
        return -1;
    }
    for (size_t i = 3; i < len; ++i) {
        if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n') {
            return (ssize_t)(i - 3);
        }
    }
    return -1;
}

static int parse_content_length(const char *value, size_t *out_len) {
    if (value == NULL || *value == '\0') {
        return -1;
    }

    size_t total = 0;
    for (const char *p = value; *p != '\0'; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return -1;
        }
        int digit = *p - '0';
        if (total > (SIZE_MAX - (size_t)digit) / 10U) {
            return -1;
        }
        total = total * 10U + (size_t)digit;
        if (total > HTTP_MAX_CONTENT_LENGTH) {
            return -2;
        }
    }

    *out_len = total;
    return 0;
}

http_parse_result_t http_parse_request(
    const char *buf,
    size_t len,
    http_request_t *out,
    size_t *consumed,
    int *error_status
) {
    if (out == NULL || consumed == NULL || error_status == NULL) {
        return HTTP_PARSE_ERROR;
    }

    memset(out, 0, sizeof(*out));
    *consumed = 0;
    *error_status = 400;

    ssize_t header_start = find_header_end(buf, len);
    if (header_start < 0) {
        return HTTP_PARSE_INCOMPLETE;
    }

    size_t header_end = (size_t)header_start;
    size_t header_block_len = header_end + 4;

    size_t line_end = SIZE_MAX;
    for (size_t i = 1; i < header_block_len; ++i) {
        if (buf[i - 1] == '\r' && buf[i] == '\n') {
            line_end = i - 1;
            break;
        }
    }

    if (line_end == SIZE_MAX || line_end == 0) {
        *error_status = 400;
        return HTTP_PARSE_ERROR;
    }

    if (line_end >= 4096) {
        *error_status = 414;
        return HTTP_PARSE_ERROR;
    }

    char line[4096];
    memcpy(line, buf, line_end);
    line[line_end] = '\0';

    char *sp1 = strchr(line, ' ');
    if (sp1 == NULL) {
        *error_status = 400;
        return HTTP_PARSE_ERROR;
    }
    char *sp2 = strchr(sp1 + 1, ' ');
    if (sp2 == NULL) {
        *error_status = 400;
        return HTTP_PARSE_ERROR;
    }
    if (strchr(sp2 + 1, ' ') != NULL) {
        *error_status = 400;
        return HTTP_PARSE_ERROR;
    }

    *sp1 = '\0';
    *sp2 = '\0';

    if (strlen(line) > HTTP_MAX_METHOD_LEN || strlen(sp1 + 1) > HTTP_MAX_PATH_LEN || strlen(sp2 + 1) > HTTP_MAX_VERSION_LEN) {
        *error_status = 414;
        return HTTP_PARSE_ERROR;
    }

    strcpy(out->method, line);
    strcpy(out->path, sp1 + 1);
    strcpy(out->version, sp2 + 1);

    if (strcmp(out->version, "HTTP/1.1") != 0) {
        *error_status = 505;
        return HTTP_PARSE_ERROR;
    }

    size_t content_length = 0;
    bool saw_content_length = false;
    bool connection_close = false;
    size_t header_count = 0;

    size_t pos = line_end + 2;
    while (pos < header_block_len) {
        size_t hdr_line_end = SIZE_MAX;
        for (size_t i = pos + 1; i < header_block_len; ++i) {
            if (buf[i - 1] == '\r' && buf[i] == '\n') {
                hdr_line_end = i - 1;
                break;
            }
        }

        if (hdr_line_end == SIZE_MAX) {
            *error_status = 400;
            return HTTP_PARSE_ERROR;
        }

        size_t hdr_len = hdr_line_end - pos;
        if (hdr_len == 0) {
            break;
        }

        const char *colon = memchr(buf + pos, ':', hdr_len);
        if (colon == NULL) {
            *error_status = 400;
            return HTTP_PARSE_ERROR;
        }

        size_t name_len = (size_t)(colon - (buf + pos));
        size_t value_len = hdr_len - name_len - 1;

        if (name_len == 0 || name_len > HTTP_MAX_HEADER_NAME_LEN || value_len > HTTP_MAX_HEADER_VALUE_LEN) {
            *error_status = 431;
            return HTTP_PARSE_ERROR;
        }

        char name[HTTP_MAX_HEADER_NAME_LEN + 1];
        char value[HTTP_MAX_HEADER_VALUE_LEN + 1];
        memcpy(name, buf + pos, name_len);
        name[name_len] = '\0';

        memcpy(value, colon + 1, value_len);
        value[value_len] = '\0';

        const char *trimmed = util_trim_left(value);
        memmove(value, trimmed, strlen(trimmed) + 1);
        util_trim_right(value);

        if (util_ascii_casecmp(name, "Content-Length") == 0) {
            size_t parsed = 0;
            int rc = parse_content_length(value, &parsed);
            if (rc == -2) {
                *error_status = 413;
                return HTTP_PARSE_ERROR;
            }
            if (rc != 0) {
                *error_status = 400;
                return HTTP_PARSE_ERROR;
            }
            if (saw_content_length && content_length != parsed) {
                *error_status = 400;
                return HTTP_PARSE_ERROR;
            }
            saw_content_length = true;
            content_length = parsed;
        } else if (util_ascii_casecmp(name, "Connection") == 0) {
            if (util_ascii_casecmp(value, "close") == 0) {
                connection_close = true;
            }
        }

        ++header_count;
        if (header_count > HTTP_MAX_HEADERS) {
            *error_status = 431;
            return HTTP_PARSE_ERROR;
        }

        pos = hdr_line_end + 2;
    }

    size_t total_needed = header_block_len + content_length;
    if (total_needed < header_block_len) {
        *error_status = 400;
        return HTTP_PARSE_ERROR;
    }

    if (len < total_needed) {
        return HTTP_PARSE_INCOMPLETE;
    }

    out->content_length = content_length;
    out->body = buf + header_block_len;
    out->body_len = content_length;
    out->connection_close = connection_close;
    *consumed = total_needed;

    return HTTP_PARSE_OK;
}
