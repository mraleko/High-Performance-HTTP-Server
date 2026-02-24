#!/usr/bin/env python3
import argparse
import concurrent.futures
import random
import socket
import subprocess
import time
from typing import Dict, Tuple


def recv_exact(sock: socket.socket, n: int) -> bytes:
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise RuntimeError("socket closed before full body")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_response(sock: socket.socket) -> Tuple[int, Dict[str, str], bytes]:
    data = bytearray()
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("socket closed before headers")
        data.extend(chunk)

    header_end = data.index(b"\r\n\r\n") + 4
    head = bytes(data[:header_end])
    rest = bytes(data[header_end:])

    lines = head.decode("latin1").split("\r\n")
    status_parts = lines[0].split(" ", 2)
    if len(status_parts) < 2:
        raise RuntimeError(f"bad status line: {lines[0]!r}")
    status = int(status_parts[1])

    headers: Dict[str, str] = {}
    for line in lines[1:]:
        if not line:
            continue
        k, v = line.split(":", 1)
        headers[k.strip().lower()] = v.strip()

    content_length = int(headers.get("content-length", "0"))
    if len(rest) < content_length:
        rest += recv_exact(sock, content_length - len(rest))

    body = rest[:content_length]
    return status, headers, body


def request_once(host: str, port: int, raw: bytes) -> Tuple[int, Dict[str, str], bytes]:
    with socket.create_connection((host, port), timeout=2.0) as sock:
        sock.sendall(raw)
        return read_response(sock)


def wait_for_healthz(host: str, port: int, timeout_sec: float = 5.0) -> None:
    deadline = time.time() + timeout_sec
    req = b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
    while time.time() < deadline:
        try:
            status, _, body = request_once(host, port, req)
            if status == 200 and body == b"ok":
                return
        except Exception:
            pass
        time.sleep(0.05)
    raise RuntimeError("server failed healthz during startup")


def keep_alive_test(host: str, port: int) -> None:
    with socket.create_connection((host, port), timeout=2.0) as sock:
        req1 = b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n"
        req2 = b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello"
        sock.sendall(req1 + req2)

        status1, headers1, body1 = read_response(sock)
        if status1 != 200 or body1 != b"ok":
            raise AssertionError(f"unexpected keep-alive response #1: {status1} {body1!r}")
        if headers1.get("connection", "").lower() != "keep-alive":
            raise AssertionError("expected keep-alive connection header")

        status2, _, body2 = read_response(sock)
        if status2 != 200 or body2 != b"hello":
            raise AssertionError(f"unexpected keep-alive response #2: {status2} {body2!r}")


def connection_close_test(host: str, port: int) -> None:
    with socket.create_connection((host, port), timeout=2.0) as sock:
        req = b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        sock.sendall(req)
        status, headers, body = read_response(sock)
        if status != 200 or body != b"ok":
            raise AssertionError("bad response for connection close test")
        if headers.get("connection", "").lower() != "close":
            raise AssertionError("expected Connection: close")
        # Peer should close shortly after response flush.
        sock.settimeout(1.0)
        trailing = sock.recv(1)
        if trailing != b"":
            raise AssertionError("expected server to close socket")


def static_and_traversal_test(host: str, port: int) -> None:
    status, _, body = request_once(
        host,
        port,
        b"GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n",
    )
    if status != 200 or body != b"hello static\n":
        raise AssertionError(f"static file mismatch: status={status} body={body!r}")

    status, _, _ = request_once(
        host,
        port,
        b"GET /static/../README.md HTTP/1.1\r\nHost: localhost\r\n\r\n",
    )
    if status not in (400, 404):
        raise AssertionError(f"expected traversal block status 400/404, got {status}")


def concurrent_load_test(host: str, port: int, n: int = 300) -> None:
    def worker(i: int) -> None:
        choice = i % 3
        if choice == 0:
            status, _, body = request_once(
                host,
                port,
                b"GET /healthz HTTP/1.1\r\nHost: localhost\r\n\r\n",
            )
            if status != 200 or body != b"ok":
                raise AssertionError("healthz mismatch")
            return

        if choice == 1:
            msg = f"msg-{i}-{random.randint(0, 1_000_000)}".encode("ascii")
            req = (
                b"POST /echo HTTP/1.1\r\n"
                b"Host: localhost\r\n"
                + f"Content-Length: {len(msg)}\r\n\r\n".encode("ascii")
                + msg
            )
            status, _, body = request_once(host, port, req)
            if status != 200 or body != msg:
                raise AssertionError("echo mismatch")
            return

        status, _, body = request_once(
            host,
            port,
            b"GET /static/hello.txt HTTP/1.1\r\nHost: localhost\r\n\r\n",
        )
        if status != 200 or body != b"hello static\n":
            raise AssertionError("static mismatch")

    with concurrent.futures.ThreadPoolExecutor(max_workers=40) as pool:
        futures = [pool.submit(worker, i) for i in range(n)]
        for fut in futures:
            fut.result(timeout=5.0)


def metrics_test(host: str, port: int, min_requests: int) -> None:
    status, _, body = request_once(
        host,
        port,
        b"GET /metrics HTTP/1.1\r\nHost: localhost\r\n\r\n",
    )
    if status != 200:
        raise AssertionError(f"metrics returned status {status}")

    text = body.decode("ascii", errors="replace")
    values: Dict[str, float] = {}
    for line in text.strip().splitlines():
        parts = line.split()
        if len(parts) == 2:
            values[parts[0]] = float(parts[1])

    required = ["requests_total", "requests_per_sec", "connections_current", "bytes_in", "bytes_out"]
    for key in required:
        if key not in values:
            raise AssertionError(f"missing metric {key}")

    if values["requests_total"] < min_requests:
        raise AssertionError(
            f"requests_total too low: {values['requests_total']} < {min_requests}"
        )
    if values["requests_per_sec"] <= 0:
        raise AssertionError("requests_per_sec should be > 0 after traffic")
    if values["bytes_in"] <= 0 or values["bytes_out"] <= 0:
        raise AssertionError("byte counters were not incremented")


def pick_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--httpd", default="./httpd-debug")
    args = parser.parse_args()

    host = "127.0.0.1"
    port = pick_port()

    proc = subprocess.Popen(
        [args.httpd, "-p", str(port), "-t", "4", "-s", "tests/static", "-i", "10"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    try:
        wait_for_healthz(host, port)
        keep_alive_test(host, port)
        connection_close_test(host, port)
        static_and_traversal_test(host, port)
        concurrent_load_test(host, port, n=300)
        metrics_test(host, port, min_requests=310)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=3.0)

    print("integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
