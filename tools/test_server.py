#!/usr/bin/env python3
"""
Slow HTTP test server for http_request_async integration testing.

Endpoints:
  GET  /slow?delay=N    respond after N seconds (default 3)
  POST /echo            echo request body after 1s delay
  GET  /fast            immediate 200 response
  GET  /error           always returns 500
  GET  /unreachable     never responds (simulates timeout) — just… hangs

Run:
  python3 tools/test_server.py [port]     default port: 8765

ESPHome device should be on the same LAN. Replace TEST_SERVER_IP in the
proxy YAML with this machine's LAN IP.
"""

import http.server
import json
import sys
import time
from socketserver import ThreadingMixIn
from urllib.parse import urlparse, parse_qs

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8765


class SlowHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Override to add timestamps
        ts = time.strftime("%H:%M:%S")
        print(f"[{ts}] {self.address_string()} - {fmt % args}")

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length else b""

    def _send(self, status, body, content_type="text/plain"):
        encoded = body.encode() if isinstance(body, str) else body
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("X-Server", "http_request_async-test")
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self):
        parsed = urlparse(self.path)
        params = parse_qs(parsed.query)
        path = parsed.path

        if path == "/slow":
            delay = float(params.get("delay", ["3"])[0])
            print(f"  → /slow  delay={delay}s  (sleeping…)")
            t0 = time.monotonic()
            time.sleep(delay)
            elapsed = time.monotonic() - t0
            self._send(200, json.dumps({
                "path": "/slow",
                "delay_requested": delay,
                "delay_actual": round(elapsed, 3),
                "message": "Still async on your end?",
            }), "application/json")

        elif path == "/fast":
            self._send(200, json.dumps({"path": "/fast", "ts": time.time()}),
                       "application/json")

        elif path == "/error":
            print("  → /error  returning 500")
            self._send(500, json.dumps({"error": "intentional 500"}),
                       "application/json")

        elif path == "/unreachable":
            print("  → /unreachable  hanging forever (simulates timeout)")
            # Block until the client times out — never send a response.
            try:
                time.sleep(3600)
            except Exception:
                pass

        else:
            self._send(404, f"Unknown path: {path}")

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        body = self._read_body()

        if path == "/echo":
            delay = 1.0
            print(f"  → /echo  body={body[:120]}  delay={delay}s")
            time.sleep(delay)
            response = {
                "path": "/echo",
                "received": body.decode(errors="replace"),
                "content_type": self.headers.get("Content-Type", ""),
            }
            self._send(200, json.dumps(response), "application/json")

        else:
            self._send(404, f"Unknown path: {path}")


class ThreadingHTTPServer(ThreadingMixIn, http.server.HTTPServer):
    """Each request runs in its own thread — /unreachable won't block the others."""
    daemon_threads = True


if __name__ == "__main__":
    server = ThreadingHTTPServer(("0.0.0.0", PORT), SlowHandler)
    print(f"Slow test server listening on port {PORT}")
    print(f"Endpoints:")
    print(f"  GET  http://<your-ip>:{PORT}/slow?delay=3   (adjustable delay)")
    print(f"  POST http://<your-ip>:{PORT}/echo           (echoes body, 1s delay)")
    print(f"  GET  http://<your-ip>:{PORT}/fast           (immediate)")
    print(f"  GET  http://<your-ip>:{PORT}/error          (always 500)")
    print(f"  GET  http://<your-ip>:{PORT}/unreachable    (hangs = timeout test)")
    print()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
