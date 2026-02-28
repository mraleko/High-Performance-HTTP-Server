#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-8080}"
THREADS="${THREADS:-2}"
STATIC_ROOT="${STATIC_ROOT:-$ROOT_DIR/tests/static}"
IDLE_TIMEOUT="${IDLE_TIMEOUT:-10}"
HTTPD_BIN="${HTTPD_BIN:-$ROOT_DIR/httpd-debug}"
LOG_FILE="${LOG_FILE:-/tmp/httpd-demo.log}"

cd "$ROOT_DIR"
make debug >/dev/null

"$HTTPD_BIN" -p "$PORT" -t "$THREADS" -s "$STATIC_ROOT" -i "$IDLE_TIMEOUT" >"$LOG_FILE" 2>&1 &
PID=$!
cleanup() {
  kill "$PID" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
}
trap cleanup EXIT

ok=0
for _ in $(seq 1 50); do
  if curl -fsS "http://127.0.0.1:${PORT}/healthz" >/dev/null; then
    ok=1
    break
  fi
  if ! kill -0 "$PID" 2>/dev/null; then
    break
  fi
  sleep 0.1
done

if [[ "$ok" -ne 1 ]]; then
  echo "server failed to start" >&2
  cat "$LOG_FILE" >&2 || true
  exit 1
fi

curl -sS "http://127.0.0.1:${PORT}/healthz"
echo
curl -sS -X POST --data-binary "hello-demo" "http://127.0.0.1:${PORT}/echo"
echo
curl -sS "http://127.0.0.1:${PORT}/static/hello.txt"
echo
curl -sS "http://127.0.0.1:${PORT}/metrics"
