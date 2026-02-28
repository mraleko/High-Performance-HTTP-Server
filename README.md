# High-Performance HTTP/1.1 Server (`httpd`)

Educational, production-oriented HTTP/1.1 server in C for Linux using non-blocking sockets + edge-triggered epoll.

## Features

- Single binary: `httpd`
- Multi-threaded event loops (`-t N`) with `SO_REUSEPORT`
- One epoll fd + one connection table per worker thread
- Correct ET handling: read/write loops drain until `EAGAIN`
- HTTP/1.1 request line + headers parsing with incremental reads
- Keep-alive by default; `Connection: close` honored
- `Content-Length` body support for `POST /echo`
- Routes:
  - `GET /healthz` -> `ok`
  - `POST /echo` -> echoes request body
  - `GET /static/<path>` -> static files via `sendfile()`
  - `GET /metrics` -> Prometheus-style text metrics (`requests_total`, `requests_per_sec`, `connections_current`, `bytes_in`, `bytes_out`)
- Static path traversal protection (`..`, absolute/empty segments rejected)
- Idle keep-alive timeout (default: 10s)
- No external deps (libc + pthreads only)

## Architecture Diagram (ASCII)

```text
                           +---------------------------+
                           |     Linux TCP stack       |
                           +-------------+-------------+
                                         |
                      SO_REUSEPORT (same IP:port on N sockets)
                   +---------------------+---------------------+
                   |                     |                     |
          +--------v--------+   +--------v--------+    ... +--v-----------+
          | Worker Thread 0 |   | Worker Thread 1 |        | Worker N-1   |
          | epoll fd (ET)   |   | epoll fd (ET)   |        | epoll fd (ET)|
          | listen fd       |   | listen fd       |        | listen fd    |
          | conn table[fd]  |   | conn table[fd]  |        | conn table   |
          +--------+--------+   +--------+--------+        +------+-------+
                   |                     |                        |
                   +---------- parse + route + response ---------+
                                         |
          +------------------------------+------------------------------+
          | /healthz | /echo | /static/<path> (sendfile) | /metrics     |
          +------------------------------+------------------------------+
```

## Project Layout

- `include/`
- `src/core`
- `src/http`
- `src/net`
- `src/util`
- `tests/`

## Build

```bash
make           # release build -> ./httpd
make debug     # debug build -> ./httpd-debug
```

## Run

```bash
./httpd -p 8080 -t 4 -s ./tests/static -i 10
```

Options:

- `-p <port>`: listen port (default `8080`)
- `-t <threads>`: number of event-loop threads (default `1`)
- `-s <static_root>`: static files root (default `./static`)
- `-i <seconds>`: idle timeout for keep-alive connections (default `10`)

## Demo

Native Linux:

```bash
bash scripts/demo_linux.sh
# or:
make demo
```

macOS via Docker (Linux runtime):

```bash
bash scripts/demo_docker.sh
# or:
make demo-docker
```

## Tests

```bash
make test
```

This runs:

- C unit tests for HTTP parser (`tests/parser_tests.c`)
- Python integration test with concurrent traffic (`tests/integration_test.py`)

Note: integration tests require Linux because the server runtime uses `epoll`.

## Benchmarking

Start server first (release recommended):

```bash
make
./httpd -p 8080 -t 4 -s ./tests/static -i 10
```

Automated benchmark script:

```bash
make bench
# or:
bash tests/benchmark.sh
```

The script runs `wrk --latency`, captures p99 for static workload, and enforces:

- static throughput `>= 75k req/s`
- static p99 latency `<= 10ms`

By default it writes `tests/benchmark_results_<timestamp>.txt`.
Committed reports:

- `tests/benchmark_results_20260224T231228Z.txt` (recorded run)
- `tests/benchmark_results_sample_2026-02-24.txt` (tracked sample copy)

### wrk

```bash
wrk -t8 -c256 -d30s http://127.0.0.1:8080/healthz
wrk -t8 -c256 -d30s -s tests/wrk_echo.lua http://127.0.0.1:8080/echo
```

Example `tests/wrk_echo.lua`:

```lua
wrk.method = "POST"
wrk.body   = "hello from wrk"
wrk.headers["Content-Length"] = tostring(#wrk.body)
```

### hey

```bash
hey -n 200000 -c 256 http://127.0.0.1:8080/healthz
hey -n 100000 -c 128 -m POST -d 'hello from hey' http://127.0.0.1:8080/echo
```

## Sample Results (Recorded Linux Run)

From `tests/benchmark_results_20260224T231228Z.txt`:

- `wrk /healthz`: `394,523 req/s` (p99 `2.12ms`)
- `wrk /static/hello.txt`: `341,518 req/s` (p99 `1.73ms`)
- `wrk /echo`: `384,212 req/s` (p99 `2.53ms`)
- benchmark assertions: `assert_static_rps_gte_75000 PASS`, `assert_static_p99_ms_lte_10 PASS`

## Design Tradeoffs

- Edge-triggered epoll gives high throughput and fewer wakeups, but requires strict drain-until-`EAGAIN` loops to avoid stalls.
- Per-thread listeners with `SO_REUSEPORT` remove accept-lock contention, but kernel-level connection distribution can be uneven in some workloads.
- Fixed-size per-connection input buffer simplifies parsing and avoids realloc churn, but increases memory use under high connection counts.
- Parser accepts `Content-Length` bodies and rejects malformed headers early for robustness, but intentionally does not implement chunked request decoding.
- Path traversal protection is lexical (`..`, absolute paths, empty segments, backslashes) for speed and clarity, but does not attempt symlink canonicalization.
- Idle timeout is enforced by periodic scans (1s granularity), which is simple and predictable but less precise than a timer wheel.

## Notes

- Linux-only implementation (`epoll`, `sendfile`, `accept4`)
- No TLS and no HTTP/2 by design
- No chunked request parsing; `POST /echo` uses `Content-Length`
