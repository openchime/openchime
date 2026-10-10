#!/usr/bin/env python3
"""Serve the web client's build directory for development (docs/WEB.md).

    scripts/webdev.py [dir] [port]

A static server with the two headers a page needs to run WebAssembly threads
(cross-origin isolation, for SharedArrayBuffer), which python's own server
does not send. Development only; a deployment serves the files from the
daemon's origin.
"""
import http.server
import os
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
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
