/* The web client the daemon serves (WEB.md, CLIENT.md §4): the client built
 * by `make web` for the browser, served under /app/ with the cross-origin
 * isolation headers wasm threads need. A daemon always serves the client that
 * matches it: the page and the loader (openchime.html, openchime.js) are
 * compiled into the binary as resources (scripts/build/embed_res.py), and the one
 * thing outside it is the wasm binary, shipped beside the executable as the
 * voice data is (`web/openchime.wasm` beside the executable, or
 * /usr/share/openchime/web when installed), read once at startup. The two
 * are built and released together, so there is nothing to configure and no
 * version to mismatch. Without the wasm beside it the routes are absent and
 * the landing page stands as before.
 *
 * Sent small, and once: the loader is compiled in gzipped too, and the build
 * writes `openchime.wasm.gz` beside the wasm, each sent with Content-Encoding to
 * a browser that takes gzip (the daemon compresses nothing itself). Every
 * route has an ETag, a hash of what it serves, so a browser revalidating its
 * copy (`Cache-Control: no-cache`) is answered 304 and downloads nothing, and
 * a new daemon -- a new client -- is never mistaken for the old one. */
#ifndef OC_WEBAPP_H
#define OC_WEBAPP_H

#include <stddef.h>
#include "http.h"

#define OC_WEBAPP_SYSTEM_DIR "/usr/share/openchime/web"
#define OC_WEBAPP_PATH       "/app/"

/* Whether this daemon carries the client at all: the loader is compiled in
 * when the daemon is built with WEB=1 (`make full`), and is empty otherwise. */
int oc_webapp_built(void);

/* Find and read the wasm beside the binary. 1 when it is there; 0 with `err`
 * filled when not (which is not an error for the daemon: it runs without a
 * web client), as it is when the client was not built in at all. */
int oc_webapp_load(const char *exe_dir, char *err, size_t errcap);
/* Whether `gz` is a gzip member whose trailer says it holds exactly `body`
 * (its CRC-32 and length): a copy left from an older build is not served. */
int oc_webapp_gzip_matches(const void *gz, size_t gz_len, const void *body, size_t body_len);
int oc_webapp_loaded(void);

/* The routes that serve it, to append to a site's: the page, the loader and
 * the wasm, and "/" redirecting to /app/. */
const oc_http_route *oc_webapp_routes(size_t *n);

/* Whether `uri` (not NUL-terminated) is the app's own redirect for a sign-in
 * (AUTH.md §8.10): the origin the page was served from, at /app/ exactly.
 * `host` is the Host the page's connection was upgraded with. */
int oc_webapp_is_redirect(const char *uri, size_t n, const char *host);

#endif
