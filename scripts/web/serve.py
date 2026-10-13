#!/usr/bin/env python3
"""Serve the web client's build directory for development (docs/WEB.md).

    scripts/web/serve.py [dir] [port]

A static server with the two headers a page needs to run WebAssembly threads
(cross-origin isolation, for SharedArrayBuffer), which python's own server
does not send. Development only; a deployment serves the files from the
daemon's origin. The page is served in development mode, as a development
daemon serves it (daemon/webapp.c), so the harness's inputs work.
"""
import http.server
import os
import sys


MODE_MARK = b"/*OC_MODE*/'normal'"
MODE_MARK_DEV = b"/*OC_MODE*/'development'"


class Handler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        path = self.translate_path(self.path)
        if not path.endswith(".html") or not os.path.isfile(path):
            return super().do_GET()
        with open(path, "rb") as f:
            body = f.read().replace(MODE_MARK, MODE_MARK_DEV, 1)
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass


def main(d, port):
    os.chdir(d)
    Handler.extensions_map.update({".wasm": "application/wasm", ".js": "text/javascript"})
    with http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler) as srv:
        print(f"serving {d} at http://127.0.0.1:{port}/", flush=True)
        srv.serve_forever()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "build/web",
         int(sys.argv[2]) if len(sys.argv) > 2 else 8765)
