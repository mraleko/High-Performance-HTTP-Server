#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HTTPD_BIN="${HTTPD:-$ROOT_DIR/httpd}"
HOST="127.0.0.1"
PORT="${PORT:-18080}"
THREADS="${THREADS:-4}"
STATIC_ROOT="${STATIC_ROOT:-$ROOT_DIR/tests/static}"
IDLE_TIMEOUT="${IDLE_TIMEOUT:-10}"
WRK_THREADS="${WRK_THREADS:-8}"
WRK_CONNECTIONS="${WRK_CONNECTIONS:-256}"
WRK_DURATION="${WRK_DURATION:-15s}"
HEY_REQUESTS="${HEY_REQUESTS:-100000}"
HEY_CONNECTIONS="${HEY_CONNECTIONS:-256}"
MIN_STATIC_RPS="${MIN_STATIC_RPS:-75000}"
MAX_STATIC_P99_MS="${MAX_STATIC_P99_MS:-10}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT_FILE="${OUT_FILE:-$ROOT_DIR/tests/benchmark_results_${STAMP}.txt}"
SERVER_LOG="${SERVER_LOG:-$ROOT_DIR/tests/benchmark_httpd_${STAMP}.log}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "benchmark.sh requires Linux (server runtime uses epoll)." >&2
  exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
  echo "curl is required." >&2
  exit 1
fi

if ! command -v wrk >/dev/null 2>&1; then
  echo "wrk is required for this benchmark script." >&2
  exit 1
fi

if [[ ! -x "$HTTPD_BIN" ]]; then
  make -C "$ROOT_DIR" release
fi

latency_to_ms() {
  local raw="$1"
  awk -v v="$raw" 'BEGIN {
    if (v ~ /us$/) { sub(/us$/, "", v); printf "%.3f", v / 1000.0; exit }
    if (v ~ /ms$/) { sub(/ms$/, "", v); printf "%.3f", v + 0.0; exit }
    if (v ~ /s$/)  { sub(/s$/, "", v);  printf "%.3f", v * 1000.0; exit }
    if (v ~ /m$/)  { sub(/m$/, "", v);  printf "%.3f", v * 60000.0; exit }
    print "-1"
  }'
}

"$HTTPD_BIN" -p "$PORT" -t "$THREADS" -s "$STATIC_ROOT" -i "$IDLE_TIMEOUT" >"$SERVER_LOG" 2>&1 &
HTTPD_PID=$!
cleanup() {
  if kill -0 "$HTTPD_PID" >/dev/null 2>&1; then
    kill "$HTTPD_PID" >/dev/null 2>&1 || true
    wait "$HTTPD_PID" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

for _ in $(seq 1 100); do
  if ! kill -0 "$HTTPD_PID" >/dev/null 2>&1; then
    break
  fi
  if [[ "$(curl -fsS "http://$HOST:$PORT/healthz" || true)" == "ok" ]]; then
    break
  fi
  sleep 0.05
done

if [[ "$(curl -fsS "http://$HOST:$PORT/healthz" || true)" != "ok" ]]; then
  echo "Server did not become healthy on $HOST:$PORT." >&2
  if ! kill -0 "$HTTPD_PID" >/dev/null 2>&1; then
    wait "$HTTPD_PID" || true
    echo "httpd process exited during startup. Log: $SERVER_LOG" >&2
  else
    echo "httpd is running but health checks failed. Log: $SERVER_LOG" >&2
  fi
  echo "----- httpd startup log -----" >&2
  tail -n 200 "$SERVER_LOG" >&2 || true
  echo "-----------------------------" >&2
  exit 1
fi

HEALTHZ_OUT="$(wrk --latency -t"$WRK_THREADS" -c"$WRK_CONNECTIONS" -d"$WRK_DURATION" "http://$HOST:$PORT/healthz")"
STATIC_OUT="$(wrk --latency -t"$WRK_THREADS" -c"$WRK_CONNECTIONS" -d"$WRK_DURATION" "http://$HOST:$PORT/static/hello.txt")"
ECHO_OUT="$(wrk --latency -t"$WRK_THREADS" -c"$WRK_CONNECTIONS" -d"$WRK_DURATION" -s "$ROOT_DIR/tests/wrk_echo.lua" "http://$HOST:$PORT/echo")"

STATIC_RPS="$(printf '%s\n' "$STATIC_OUT" | awk '/Requests\/sec:/ { gsub(/,/, "", $2); print $2; exit }')"
STATIC_P99_RAW="$(printf '%s\n' "$STATIC_OUT" | awk '$1 == "99%" { print $2; exit }')"
STATIC_P99_MS="$(latency_to_ms "$STATIC_P99_RAW")"

if [[ -z "$STATIC_RPS" || -z "$STATIC_P99_RAW" || "$STATIC_P99_MS" == "-1" ]]; then
  echo "failed to parse static benchmark output" >&2
  exit 1
fi

ASSERT_RPS_OK="$(awk -v v="$STATIC_RPS" -v t="$MIN_STATIC_RPS" 'BEGIN { if (v + 0 >= t + 0) print "PASS"; else print "FAIL" }')"
ASSERT_P99_OK="$(awk -v v="$STATIC_P99_MS" -v t="$MAX_STATIC_P99_MS" 'BEGIN { if (v + 0 <= t + 0) print "PASS"; else print "FAIL" }')"

{
  echo "# httpd benchmark results"
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "hostname: $(hostname)"
  echo "kernel: $(uname -srmo)"
  echo "server_bin: $HTTPD_BIN"
  echo "server_args: -p $PORT -t $THREADS -s $STATIC_ROOT -i $IDLE_TIMEOUT"
  echo

  echo "## wrk GET /healthz"
  echo "$HEALTHZ_OUT"
  echo

  echo "## wrk GET /static/hello.txt"
  echo "$STATIC_OUT"
  echo

  echo "## wrk POST /echo"
  echo "$ECHO_OUT"
  echo

  if command -v hey >/dev/null 2>&1; then
    echo "## hey GET /healthz"
    hey -n "$HEY_REQUESTS" -c "$HEY_CONNECTIONS" "http://$HOST:$PORT/healthz"
    echo

    echo "## hey POST /echo"
    hey -n "$HEY_REQUESTS" -c "$HEY_CONNECTIONS" -m POST -d "hello from hey" "http://$HOST:$PORT/echo"
    echo
  fi

  echo "## assertions"
  echo "static_rps $STATIC_RPS"
  echo "static_p99_ms $STATIC_P99_MS"
  echo "assert_static_rps_gte_${MIN_STATIC_RPS} $ASSERT_RPS_OK"
  echo "assert_static_p99_ms_lte_${MAX_STATIC_P99_MS} $ASSERT_P99_OK"
  echo

  echo "## /metrics snapshot"
  curl -fsS "http://$HOST:$PORT/metrics"
} | tee "$OUT_FILE"

echo
echo "saved benchmark report: $OUT_FILE"

if [[ "$ASSERT_RPS_OK" != "PASS" || "$ASSERT_P99_OK" != "PASS" ]]; then
  exit 2
fi
