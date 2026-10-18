#include "webapp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"     /* the mode the page is told */
#include "mbedtls/sha256.h"
#include "tts_data.h"   /* oc_data_dir_find: the same beside-the-binary lookup */

/* The page and the loader, compiled in (scripts/build/embed_res.py, the Makefile's
 * WEBAPP_RES). */
extern const unsigned char oc_webapp_html[];
extern const size_t        oc_webapp_html_len;
extern const unsigned char oc_webapp_js[];
extern const size_t        oc_webapp_js_len;
extern const unsigned char oc_webapp_js_gz[];   /* the same, gzipped at build */
extern const size_t        oc_webapp_js_gz_len;

/* Threads need the page cross-origin isolated (SharedArrayBuffer); the two
 * headers say so. The bundle is versioned by the daemon that serves it, so a
 * browser may keep it until the daemon changes. */
#define ISOLATION "Cross-Origin-Opener-Policy: same-origin\r\n" \
                  "Cross-Origin-Embedder-Policy: require-corp\r\n"
#define NO_CACHE  "Cache-Control: no-cache\r\n"

/* The page's mode (openchime.html): `normal` as compiled in, rewritten to
 * `development` when the daemon is, so the harness's inputs work on a page
 * from a development daemon and on no other. The page never takes its mode
 * from its own address. */
#define MODE_MARK     "/*OC_MODE*/'normal'"
#define MODE_MARK_DEV "/*OC_MODE*/'development'"

static char  *g_html;
static size_t g_html_len;
static char  *g_wasm;
static size_t g_wasm_len;
static char  *g_wasm_gz;       /* openchime.wasm.gz, when it matches the wasm */
static size_t g_wasm_gz_len;
/* Each route's ETags (its body's, its gzip copy's) and the wasm's headers. */
static char   g_etag[3][24], g_etag_gz[3][28];
static char   g_wasm_extra[256];
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

/* CRC-32 (the gzip trailer's, ISO 3309), for oc_webapp_gzip_matches. */
static uint32_t crc32_of(const unsigned char *p, size_t n) {
    static uint32_t table[256];
    static int ready;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

int oc_webapp_gzip_matches(const void *gz, size_t gz_len, const void *body, size_t body_len) {
    const unsigned char *g = gz;
    if (!g || gz_len < 18 || g[0] != 0x1F || g[1] != 0x8B || g[2] != 8) return 0;
    const unsigned char *t = g + gz_len - 8;   /* CRC-32, then the length mod 2^32, little-endian */
    uint32_t crc  = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
    uint32_t size = (uint32_t)t[4] | (uint32_t)t[5] << 8 | (uint32_t)t[6] << 16 | (uint32_t)t[7] << 24;
    return size == (uint32_t)body_len && crc == crc32_of(body, body_len);
}

/* `"<16 hex of the SHA-256>"`, with `-gz` inside the quotes for the gzip copy:
 * an ETag names one representation. */
static void etags(const void *body, size_t len, char *etag, size_t ecap, char *etag_gz, size_t gcap) {
    unsigned char h[32];
    char hex[17];
    mbedtls_sha256(body, len, h, 0);
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    snprintf(etag, ecap, "\"%s\"", hex);
    snprintf(etag_gz, gcap, "\"%s-gz\"", hex);
}

static void route(const char *path, const char *ctype, const void *body, size_t len,
                  const void *gz, size_t gz_len) {
    int i = (int)g_n_routes;
    oc_http_route *r = &g_routes[g_n_routes++];
    memset(r, 0, sizeof *r);
    r->method = "GET"; r->path = path; r->prefix = 0; r->kind = OC_HTTP_STATIC;
    r->max_body = 0; r->ctype = ctype; r->body = body; r->body_len = len;
    r->extra = ISOLATION NO_CACHE;
    etags(body, len, g_etag[i], sizeof g_etag[i], g_etag_gz[i], sizeof g_etag_gz[i]);
    r->etag = g_etag[i];
    if (gz && gz_len) { r->gz_body = gz; r->gz_len = gz_len; r->etag_gz = g_etag_gz[i]; }
}

int oc_webapp_built(void) { return oc_webapp_js_len > 0; }

int oc_webapp_load(const char *exe_dir, char *err, size_t errcap) {
    char dir[1024], path[1200];
    if (!oc_webapp_built()) {
        if (err && errcap) snprintf(err, errcap, "the web client was not built into this daemon (make WEB=1, or make full)");
        return 0;
    }
    if (!oc_data_dir_find(OC_WEBAPP_SYSTEM_DIR, exe_dir, "web", dir, sizeof dir, err, errcap))
        return 0;
    snprintf(path, sizeof path, "%s/openchime.wasm", dir);
    g_wasm = read_file(path, &g_wasm_len);
    if (!g_wasm) {
        if (err && errcap) snprintf(err, errcap, "%s: cannot read", path);
        return 0;
    }
    /* The gzip copy the build wrote beside it, if it is of this wasm. One
     * left from another build would send a browser the wrong client, so it
     * is passed over, and the wasm goes uncompressed. */
    snprintf(path, sizeof path, "%s/openchime.wasm.gz", dir);
    g_wasm_gz = read_file(path, &g_wasm_gz_len);
    if (g_wasm_gz && !oc_webapp_gzip_matches(g_wasm_gz, g_wasm_gz_len, g_wasm, g_wasm_len)) {
        fprintf(stderr, "web client: %s is not of openchime.wasm beside it; sending the wasm uncompressed\n", path);
        free(g_wasm_gz); g_wasm_gz = NULL; g_wasm_gz_len = 0;
    }
    const void *html = oc_webapp_html;
    size_t html_len = oc_webapp_html_len;
    if (oc_config_get()->mode == OC_MODE_DEVELOPMENT) {
        const char *page = (const char *)oc_webapp_html;
        const char *at = strstr(page, MODE_MARK);
        size_t ml = sizeof MODE_MARK - 1, dl = sizeof MODE_MARK_DEV - 1;
        if (!at || !(g_html = malloc(oc_webapp_html_len - ml + dl))) {
            if (err && errcap) snprintf(err, errcap, "the page cannot be told development mode");
            free(g_wasm); g_wasm = NULL;
            free(g_wasm_gz); g_wasm_gz = NULL; g_wasm_gz_len = 0;
            return 0;
        }
        size_t pre = (size_t)(at - page);
        memcpy(g_html, page, pre);
        memcpy(g_html + pre, MODE_MARK_DEV, dl);
        memcpy(g_html + pre + dl, at + ml, oc_webapp_html_len - pre - ml);
        g_html_len = oc_webapp_html_len - ml + dl;
        html = g_html; html_len = g_html_len;
    }
    g_n_routes = 0;
    route(OC_WEBAPP_PATH,                  "text/html; charset=utf-8",       html,         html_len, NULL, 0);
    route(OC_WEBAPP_PATH "openchime.js",   "text/javascript; charset=utf-8", oc_webapp_js, oc_webapp_js_len,
          oc_webapp_js_gz, oc_webapp_js_gz_len);
    route(OC_WEBAPP_PATH "openchime.wasm", "application/wasm",               g_wasm,       g_wasm_len,
          g_wasm_gz, g_wasm_gz_len);
    /* The wasm's size as it will be once decoded, for the page's progress: with
     * Content-Encoding, Content-Length is the compressed size. */
    snprintf(g_wasm_extra, sizeof g_wasm_extra, ISOLATION NO_CACHE "X-OC-Size: %zu\r\n", g_wasm_len);
    g_routes[g_n_routes - 1].extra = g_wasm_extra;
    /* The front door: a browser at the workspace's address gets the client. */
    oc_http_route *r = &g_routes[g_n_routes++];
    memset(r, 0, sizeof *r);
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
