#include <stdio.h>
#include <string.h>

#include "http_parser.h"

static int g_failures = 0;

#define CHECK(expr)                                                                                 \
    do {                                                                                            \
        if (!(expr)) {                                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                      \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

static void test_basic_get(void) {
    const char *req =
        "GET /healthz HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http_request_t parsed;
    size_t consumed = 0;
    int status = 0;

    http_parse_result_t rc = http_parse_request(req, strlen(req), &parsed, &consumed, &status);
    CHECK(rc == HTTP_PARSE_OK);
    CHECK(consumed == strlen(req));
    CHECK(strcmp(parsed.method, "GET") == 0);
    CHECK(strcmp(parsed.path, "/healthz") == 0);
    CHECK(parsed.content_length == 0);
    CHECK(parsed.connection_close == false);
}

static void test_partial_headers(void) {
    const char *full =
        "GET /healthz HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Test: abc\r\n"
        "\r\n";

    http_request_t parsed;
    size_t consumed = 0;
    int status = 0;

    size_t half = strlen(full) - 3;
    http_parse_result_t rc = http_parse_request(full, half, &parsed, &consumed, &status);
    CHECK(rc == HTTP_PARSE_INCOMPLETE);

    rc = http_parse_request(full, strlen(full), &parsed, &consumed, &status);
    CHECK(rc == HTTP_PARSE_OK);
    CHECK(consumed == strlen(full));
}

static void test_partial_body(void) {
    const char *hdr =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n";
    const char *body = "hello";

    char req[256];
    snprintf(req, sizeof(req), "%s%s", hdr, body);

    http_request_t parsed;
    size_t consumed = 0;
    int status = 0;

    size_t short_len = strlen(hdr) + 2;
    http_parse_result_t rc = http_parse_request(req, short_len, &parsed, &consumed, &status);
    CHECK(rc == HTTP_PARSE_INCOMPLETE);

    rc = http_parse_request(req, strlen(req), &parsed, &consumed, &status);
    CHECK(rc == HTTP_PARSE_OK);
    if (rc == HTTP_PARSE_OK) {
        CHECK(parsed.body_len == 5);
        CHECK(memcmp(parsed.body, "hello", 5) == 0);
    }
}

static void test_invalid_header(void) {
    const char *req =
        "GET /healthz HTTP/1.1\r\n"
        "Host localhost\r\n"
        "\r\n";

    http_request_t parsed;
    size_t consumed = 0;
    int status = 0;
    http_parse_result_t rc = http_parse_request(req, strlen(req), &parsed, &consumed, &status);
    CHECK(rc == HTTP_PARSE_ERROR);
    CHECK(status == 400);
}

static void test_duplicate_content_length_mismatch(void) {
    const char *req =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 4\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello";

    http_request_t parsed;
    size_t consumed = 0;
    int status = 0;
    http_parse_result_t rc = http_parse_request(req, strlen(req), &parsed, &consumed, &status);
    CHECK(rc == HTTP_PARSE_ERROR);
    CHECK(status == 400);
}

static void test_connection_close_header(void) {
    const char *req =
        "GET /healthz HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: Close\r\n"
        "\r\n";

    http_request_t parsed;
    size_t consumed = 0;
    int status = 0;

    http_parse_result_t rc = http_parse_request(req, strlen(req), &parsed, &consumed, &status);
    CHECK(rc == HTTP_PARSE_OK);
    CHECK(parsed.connection_close == true);
}

static void test_too_many_headers(void) {
    char req[8192];
    size_t pos = 0;
    pos += (size_t)snprintf(req + pos, sizeof(req) - pos, "GET /healthz HTTP/1.1\r\n");
    for (int i = 0; i < 70; ++i) {
        pos += (size_t)snprintf(req + pos, sizeof(req) - pos, "X-%d: y\r\n", i);
    }
    pos += (size_t)snprintf(req + pos, sizeof(req) - pos, "\r\n");

    http_request_t parsed;
    size_t consumed = 0;
    int status = 0;

    http_parse_result_t rc = http_parse_request(req, pos, &parsed, &consumed, &status);
    CHECK(rc == HTTP_PARSE_ERROR);
    CHECK(status == 431);
}

static void test_http_version_not_supported(void) {
    const char *req =
        "GET /healthz HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "\r\n";

    http_request_t parsed;
    size_t consumed = 0;
    int status = 0;

    http_parse_result_t rc = http_parse_request(req, strlen(req), &parsed, &consumed, &status);
    CHECK(rc == HTTP_PARSE_ERROR);
    CHECK(status == 505);
}

int main(void) {
    test_basic_get();
    test_partial_headers();
    test_partial_body();
    test_invalid_header();
    test_duplicate_content_length_mismatch();
    test_connection_close_header();
    test_too_many_headers();
    test_http_version_not_supported();

    if (g_failures == 0) {
        printf("parser tests passed\n");
        return 0;
    }

    fprintf(stderr, "parser tests failed: %d failure(s)\n", g_failures);
    return 1;
}
