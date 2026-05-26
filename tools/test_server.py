#!/usr/bin/env python3
"""
HTTP test server for http_request_async hardware testing.

Endpoints:
  GET  /slow?delay=N        respond after N seconds (default 3)
  GET  /fast                immediate 200 with X-Server header
  GET  /bytes?size=N        N bytes of data with Content-Length: N
  GET  /stream?lines=N      N JSON lines via chunked encoding (no Content-Length)
  GET  /headers             echo all request headers as JSON
  GET  /redirect?n=N        chain of N redirects ending at /fast
  GET  /status?code=N       respond with HTTP status code N, empty body
  GET  /error               always returns 500 (kept for compatibility)
  GET  /unreachable         hangs forever — simulates a timeout
  POST /echo                echo request body after 1 s delay
  DELETE /delete            returns 200
  PATCH /patch              returns 200 + echoes body

Run:
  python3 tools/test_server.py [port]     default port: 8765
"""

import http.server
import json
import os
import sys
import time
from socketserver import ThreadingMixIn
from urllib.parse import urlparse, parse_qs

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765

X_SERVER = "http_request_async-test"


class TestHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {self.address_string()} {self.command} {self.path} — {fmt % args}")

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length else b""

    def _send(self, status, body, content_type="application/json"):
        """Send a normal response with Content-Length."""
        encoded = body.encode() if isinstance(body, str) else body
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("X-Server", X_SERVER)
        self.end_headers()
        self.wfile.write(encoded)

    def _send_chunked(self, body, content_type="application/json"):
        """Send a chunked response — no Content-Length header."""
        encoded = body.encode() if isinstance(body, str) else body
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("X-Server", X_SERVER)
        self.end_headers()
        # One chunk containing the entire body
        self.wfile.write(f"{len(encoded):x}\r\n".encode())
        self.wfile.write(encoded)
        self.wfile.write(b"\r\n")
        # Terminating empty chunk
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def _parsed(self):
        parsed = urlparse(self.path)
        return parsed.path, parse_qs(parsed.query)

    # ── GET ───────────────────────────────────────────────────────────────────

    def do_GET(self):
        path, params = self._parsed()

        if path == "/slow":
            delay = float(params.get("delay", ["3"])[0])
            print(f"  → sleeping {delay}s")
            t0 = time.monotonic()
            time.sleep(delay)
            self._send(200, json.dumps({
                "path": "/slow",
                "delay_requested": delay,
                "delay_actual": round(time.monotonic() - t0, 3),
            }))

        elif path == "/fast":
            self._send(200, json.dumps({"path": "/fast", "ts": time.time()}))

        elif path == "/bytes":
            size = int(params.get("size", ["1024"])[0])
            self._send(200, os.urandom(size), "application/octet-stream")

        elif path == "/stream":
            lines = int(params.get("lines", ["5"])[0])
            body = "\n".join(
                json.dumps({"line": i, "ts": time.time()}) for i in range(lines)
            ) + "\n"
            self._send_chunked(body, "application/x-ndjson")

        elif path == "/headers":
            echoed = {k: v for k, v in self.headers.items()}
            self._send(200, json.dumps({"headers": echoed}))

        elif path == "/redirect":
            n = int(params.get("n", ["1"])[0])
            host = self.headers.get("Host", f"localhost:{PORT}")
            if n <= 0:
                self._send(200, json.dumps({"path": "/redirect", "hops": "done"}))
            else:
                target = f"http://{host}/redirect?n={n - 1}"
                self.send_response(302)
                self.send_header("Location", target)
                self.send_header("Content-Length", "0")
                self.send_header("X-Server", X_SERVER)
                self.end_headers()

        elif path == "/status":
            code = int(params.get("code", ["200"])[0])
            self._send(code, json.dumps({"status": code}))

        elif path == "/error":
            self._send(500, json.dumps({"error": "intentional 500"}))

        elif path == "/unreachable":
            print("  → hanging forever")
            try:
                time.sleep(3600)
            except Exception:
                pass

        else:
            self._send(404, json.dumps({"error": f"unknown path: {path}"}))

    # ── POST ──────────────────────────────────────────────────────────────────

    def do_POST(self):
        path, _ = self._parsed()
        body = self._read_body()

        if path == "/echo":
            time.sleep(1.0)
            self._send(200, json.dumps({
                "path": "/echo",
                "received": body.decode(errors="replace"),
                "content_type": self.headers.get("Content-Type", ""),
            }))
        else:
            self._send(404, json.dumps({"error": f"unknown path: {path}"}))

    # ── DELETE ────────────────────────────────────────────────────────────────

    def do_DELETE(self):
        path, _ = self._parsed()
        if path == "/delete":
            self._send(200, json.dumps({"path": "/delete", "deleted": True}))
        else:
            self._send(404, json.dumps({"error": f"unknown path: {path}"}))

    # ── PATCH ─────────────────────────────────────────────────────────────────

    def do_PATCH(self):
        path, _ = self._parsed()
        body = self._read_body()
        if path == "/patch":
            self._send(200, json.dumps({
                "path": "/patch",
                "received": body.decode(errors="replace"),
            }))
        else:
            self._send(404, json.dumps({"error": f"unknown path: {path}"}))


class ThreadingHTTPServer(ThreadingMixIn, http.server.HTTPServer):
    """Each request in its own thread so /unreachable doesn't block others."""
    daemon_threads = True


if __name__ == "__main__":
    server = ThreadingHTTPServer(("0.0.0.0", PORT), TestHandler)
    print(f"http_request_async test server on port {PORT}")
    print(f"  GET  /slow?delay=N       slow response")
    print(f"  GET  /fast               immediate 200 + X-Server header")
    print(f"  GET  /bytes?size=N       N bytes with Content-Length")
    print(f"  GET  /stream?lines=N     chunked response (no Content-Length)")
    print(f"  GET  /headers            echo request headers as JSON")
    print(f"  GET  /redirect?n=N       N-hop redirect chain")
    print(f"  GET  /status?code=N      return that HTTP status code")
    print(f"  GET  /error              always 500")
    print(f"  GET  /unreachable        hangs forever (timeout test)")
    print(f"  POST /echo               echo body (1 s delay)")
    print(f"  DELETE /delete           200")
    print(f"  PATCH /patch             200 + echo body")
    print()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
