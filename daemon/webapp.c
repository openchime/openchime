#include "webapp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tts_data.h"   /* oc_data_dir_find: the same beside-the-binary lookup */

/* The page and the loader, compiled in (scripts/embed_res.py, the Makefile's
 * WEBAPP_RES). */
extern const unsigned char oc_webapp_html[];
extern const size_t        oc_webapp_html_len;
extern const unsigned char oc_webapp_js[];
extern const size_t        oc_webapp_js_len;

/* Threads need the page cross-origin isolated (SharedArrayBuffer); the two
 * headers say so. The bundle is versioned by the daemon that serves it, so a
 * browser may keep it until the daemon changes. */
#define ISOLATION "Cross-Origin-Opener-Policy: same-origin\r\n" \
                  "Cross-Origin-Embedder-Policy: require-corp\r\n"
#define NO_CACHE  "Cache-Control: no-cache\r\n"

static char  *g_wasm;
static size_t g_wasm_len;
static oc_http_route g_routes[4];
static size_t g_n_routes;
static int g_loaded;

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) { fclose(f); return NULL; }
    char *d = malloc((size_t)st.st_size);
    if (!d) { fclose(f); return NULL; }
    size_t got = fread(d, 1, (size_t)st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) { free(d); return NULL; }
    *len = got;
    return d;
}

static void route(const char *path, const char *ctype, const void *body, size_t len) {
    oc_http_route *r = &g_routes[g_n_routes++];
    r->method = "GET"; r->path = path; r->prefix = 0; r->kind = OC_HTTP_STATIC;
    r->max_body = 0; r->ctype = ctype; r->body = body; r->body_len = len;
    r->extra = ISOLATION NO_CACHE;
}

int oc_webapp_load(const char *exe_dir, char *err, size_t errcap) {
    char dir[1024], path[1200];
    if (!oc_data_dir_find("web", NULL, OC_WEBAPP_SYSTEM_DIR, exe_dir, "web", dir, sizeof dir, err, errcap))
        return 0;
    snprintf(path, sizeof path, "%s/openchime.wasm", dir);
    g_wasm = read_file(path, &g_wasm_len);
    if (!g_wasm) {
        if (err && errcap) snprintf(err, errcap, "%s: cannot read", path);
        return 0;
    }
    g_n_routes = 0;
    route(OC_WEBAPP_PATH,                  "text/html; charset=utf-8",       oc_webapp_html, oc_webapp_html_len);
    route(OC_WEBAPP_PATH "openchime.js",   "text/javascript; charset=utf-8", oc_webapp_js,   oc_webapp_js_len);
    route(OC_WEBAPP_PATH "openchime.wasm", "application/wasm",               g_wasm,         g_wasm_len);
    /* The front door: a browser at the workspace's address gets the client. */
    oc_http_route *r = &g_routes[g_n_routes++];
    r->method = "GET"; r->path = "/"; r->prefix = 0; r->kind = OC_HTTP_REDIRECT;
    r->max_body = 0; r->ctype = NULL; r->body = OC_WEBAPP_PATH; r->body_len = 0; r->extra = NULL;
    g_loaded = 1;
    return 1;
}

int oc_webapp_loaded(void) { return g_loaded; }

const oc_http_route *oc_webapp_routes(size_t *n) {
    *n = g_loaded ? g_n_routes : 0;
    return g_routes;
}

int oc_webapp_is_redirect(const char *uri, size_t n, const char *host) {
    if (!g_loaded || !uri || !host || !host[0]) return 0;
    /* The app is served on the TLS port only: its origin is https. */
    static const char SCHEME[] = "https://";
    size_t sl = sizeof SCHEME - 1, hl = strlen(host), pl = strlen(OC_WEBAPP_PATH);
    return n == sl + hl + pl && memcmp(uri, SCHEME, sl) == 0 && memcmp(uri + sl, host, hl) == 0 &&
           memcmp(uri + sl + hl, OC_WEBAPP_PATH, pl) == 0;
}
