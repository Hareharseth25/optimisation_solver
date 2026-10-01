"""Local HTTP transport for the Explorer application boundary.

    GET  /api/health   -> {"status": "ok", "contract": "optimsolver.solve.v1"}
    POST /api/solve    -> service.solve(); see explorer/README.md
    GET  /, /<asset>   -> the Explorer UI (explorer/web), same origin as the API

This file only moves bytes between HTTP and service.solve(), and serves the
UI's static files so the browser talks to the API from the same origin (no
CORS, so no other site can drive the unauthenticated API). Static files come
from a fixed allowlist built at startup; request paths are looked up in it,
never joined onto the filesystem. Binds to the loopback interface by default.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from functools import partial
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Dict, Optional, Sequence

from . import service

WEB_ROOT = Path(__file__).resolve().parent.parent / "web"
CONTENT_TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
                 ".js": "text/javascript; charset=utf-8"}
# Everything the page may load comes from this origin; no inline script.
SECURITY_HEADERS = {
    "Content-Security-Policy": ("default-src 'self'; script-src 'self'; style-src 'self'; "
                                "img-src 'self' data:; connect-src 'self'; base-uri 'none'; "
                                "form-action 'none'; frame-ancestors 'none'"),
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "no-referrer",
}


def static_routes(root: Path = WEB_ROOT) -> Dict[str, Path]:
    """URL path -> file, for the UI's own html/css/js only (tests excluded)."""
    routes: Dict[str, Path] = {}
    if not root.is_dir():
        return routes
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        if (path.is_file() and path.suffix in CONTENT_TYPES
                and relative.parts[0] not in ("tests", "node_modules")):
            routes["/" + relative.as_posix()] = path
    if "/index.html" in routes:
        routes["/"] = routes["/index.html"]
    return routes


class ExplorerHandler(BaseHTTPRequestHandler):
    server_version = "KAIRO-Explorer/0.1"

    def __init__(self, *args, binary: str, quiet: bool, routes: Dict[str, Path], **kwargs):
        self.binary = binary
        self.quiet = quiet
        self.routes = routes
        super().__init__(*args, **kwargs)

    def log_message(self, format, *args):  # noqa: A002 (BaseHTTPRequestHandler signature)
        if not self.quiet:
            super().log_message(format, *args)

    def _send(self, status: int, body: dict) -> None:
        payload = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        for name, value in SECURITY_HEADERS.items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(payload)

    def _send_file(self, path: Path) -> None:
        payload = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", CONTENT_TYPES[path.suffix])
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        for name, value in SECURITY_HEADERS.items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(payload)

    def _error(self, status: int, outcome: str, message: str) -> None:
        self._send(status, {"outcome": outcome, "record": None,
                            "error": {"message": message}, "process": None})

    def do_GET(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path == "/api/health":
            self._send(200, {"status": "ok", "contract": service.SCHEMA})
        elif path in self.routes:
            self._send_file(self.routes[path])
        else:
            self._error(404, "not_found", "no such route")

    def do_POST(self):  # noqa: N802
        if self.path != "/api/solve":
            self._error(404, "not_found", "no such route")
            return
        content_type = self.headers.get("Content-Type", "").split(";")[0].strip()
        if content_type != "application/json":
            self._error(415, "bad_request", "Content-Type must be application/json")
            return
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            self._error(411, "bad_request", "Content-Length is required")
            return
        if length < 0 or length > service.MAX_REQUEST_BYTES:
            self._error(413, "bad_request", "request too large")
            return
        status, body = service.solve(self.rfile.read(length), self.binary)
        self._send(status, body)


def make_server(binary: str, host: str = "127.0.0.1", port: int = 8765,
                quiet: bool = False) -> ThreadingHTTPServer:
    if not (os.path.isfile(binary) and os.access(binary, os.X_OK)):
        raise FileNotFoundError(f"optimsolver binary not found or not executable: {binary}")
    handler = partial(ExplorerHandler, binary=os.path.abspath(binary), quiet=quiet,
                      routes=static_routes())
    return ThreadingHTTPServer((host, port), handler)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="KAIRO Explorer application service (local).")
    parser.add_argument("--binary", required=True, help="path to the optimsolver executable")
    parser.add_argument("--host", default="127.0.0.1",
                        help="interface to bind (default: loopback only)")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args(argv)
    server = make_server(args.binary, args.host, args.port)
    print(f"KAIRO Explorer on http://{args.host}:{server.server_address[1]}/", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0
