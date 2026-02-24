#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stdbool.h>
#include <stddef.h>

#define HTTP_MAX_METHOD_LEN 15
#define HTTP_MAX_PATH_LEN 2047
#define HTTP_MAX_VERSION_LEN 15
#define HTTP_MAX_HEADER_NAME_LEN 63
#define HTTP_MAX_HEADER_VALUE_LEN 1023
#define HTTP_MAX_HEADERS 64
#define HTTP_MAX_CONTENT_LENGTH (128 * 1024)

typedef struct {
    char method[HTTP_MAX_METHOD_LEN + 1];
    char path[HTTP_MAX_PATH_LEN + 1];
    char version[HTTP_MAX_VERSION_LEN + 1];
    size_t content_length;
    bool connection_close;
    const char *body;
    size_t body_len;
} http_request_t;

typedef enum {
    HTTP_PARSE_INCOMPLETE = 0,
    HTTP_PARSE_OK = 1,
    HTTP_PARSE_ERROR = 2
} http_parse_result_t;

http_parse_result_t http_parse_request(
    const char *buf,
    size_t len,
    http_request_t *out,
    size_t *consumed,
    int *error_status
);

#endif
