/* The daemon's HTTP stack -- see http.h. */

#include "http.h"
#include "picohttpparser.h"
#include "json.h"     /* jwt.c carries the jsmn implementation; this, the reader */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ci_eq(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return 1;
}

int oc_http_parse_head(const char *buf, size_t len, oc_http_req *req) {
    memset(req, 0, sizeof *req);
    struct phr_header h[OC_HTTP_MAX_HEADERS];
    size_t nh = OC_HTTP_MAX_HEADERS;
    int minor = 0;
    int r = phr_parse_request(buf, len, &req->method, &req->method_len, &req->path,
                              &req->path_len, &minor, h, &nh, 0);
    /* Incomplete is only incomplete while it could still become a head we take:
     * a peer must not make us hold header bytes without end. */
    if (r == -2) return len > OC_HTTP_MAX_HEAD ? -1 : 0;
    if (r < 0 || (size_t)r > OC_HTTP_MAX_HEAD) return -1;
    if (req->method_len == 0 || req->path_len == 0) return -1;
    req->head_len = (size_t)r;

    int have_len = 0;
    for (size_t i = 0; i < nh; i++) {
        if (h[i].name && ci_eq(h[i].name, h[i].name_len, "host")) {
            if (req->host) return -1;                   /* one Host (RFC 9112 §3.2) */
            req->host = h[i].value; req->host_len = h[i].value_len;
            continue;
        }
        if (h[i].name && ci_eq(h[i].name, h[i].name_len, "origin")) {
            if (req->origin) return -1;
            req->origin = h[i].value; req->origin_len = h[i].value_len;
            continue;
        }
        if (!h[i].name) continue;                       /* a folded continuation line */
        if (ci_eq(h[i].name, h[i].name_len, "upgrade")) {
            if (h[i].value_len == 9 && ci_eq(h[i].value, 9, "websocket")) req->upgrade_ws = 1;
            continue;
        }
        if (ci_eq(h[i].name, h[i].name_len, "accept-encoding")) {
            if (oc_http_accepts_gzip(h[i].value, h[i].value_len)) req->accept_gzip = 1;
            continue;
        }
        if (ci_eq(h[i].name, h[i].name_len, "if-none-match")) {
            if (!req->inm) { req->inm = h[i].value; req->inm_len = h[i].value_len; }
            continue;
        }
        if (ci_eq(h[i].name, h[i].name_len, "sec-websocket-key")) {
            req->ws_key = h[i].value; req->ws_key_len = h[i].value_len;
            continue;
        }
        if (ci_eq(h[i].name, h[i].name_len, "sec-websocket-protocol")) {
            /* The first one offered is the one taken (ioloop.c echoes it). */
            const char *v = h[i].value; size_t vl = h[i].value_len;
            while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
            size_t k = 0;
            while (k < vl && v[k] != ',' && v[k] != ' ' && v[k] != '\t') k++;
            if (k && !req->ws_proto) { req->ws_proto = v; req->ws_proto_len = k; }
            continue;
        }
        if (ci_eq(h[i].name, h[i].name_len, "content-length")) {
            if (h[i].value_len == 0 || h[i].value_len > 19) return -1;
            size_t v = 0;
            for (size_t k = 0; k < h[i].value_len; k++) {
                char ch = h[i].value[k];
                if (ch < '0' || ch > '9') return -1;
                v = v * 10 + (size_t)(ch - '0');
            }
            /* Two lengths that disagree are how a request is smuggled past a
             * proxy that believes the other one. */
            if (have_len && v != req->content_length) return -1;
            req->content_length = v;
            have_len = 1;
        } else if (ci_eq(h[i].name, h[i].name_len, "transfer-encoding")) {
            return -1;   /* no route takes a chunked body; see http.h */
        } else if (ci_eq(h[i].name, h[i].name_len, "content-type")) {
            /* application/json, optionally with a ; charset= suffix. */
            if (h[i].value_len >= 16 && ci_eq(h[i].value, 16, "application/json") &&
                (h[i].value_len == 16 || h[i].value[16] == ';' || h[i].value[16] == ' '))
                req->is_json = 1;
            if (h[i].value_len >= 33 && ci_eq(h[i].value, 33, "application/x-www-form-urlencoded") &&
                (h[i].value_len == 33 || h[i].value[33] == ';' || h[i].value[33] == ' '))
                req->is_form = 1;
        }
    }
    return 1;
}

int oc_http_parse(const char *buf, size_t len, size_t max_body, oc_http_req *req) {
    int r = oc_http_parse_head(buf, len, req);
    if (r <= 0) return r;
    if (req->content_length > max_body) return -1;
    if (len - req->head_len < req->content_length) return 0;   /* body still arriving */
    req->body = buf + req->head_len;
    req->body_len = req->content_length;
    return 1;
}

/* --- the webhook payload (ARCH-71) -------------------------------------------- */

/* What is composed so far: at most `cap` - 1 bytes, then a NUL. Past the cap
 * nothing more is taken, as a body past OC_MAX_BODY_SIZE never was. */
typedef struct { char *p; size_t n, cap; int full; } wh_out;

static void wh_put(wh_out *o, const char *s, size_t n) {
    if (o->full) return;
    if (n > o->cap - 1 - o->n) {
        /* What fits, never ending inside a character; and nothing after it, so
         * the text is cut short rather than missing a piece in its middle. */
        n = o->cap - 1 - o->n;
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        o->full = 1;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = '\0';
}

/* The bytes of the UTF-8 character starting at s[0], at most `n`. */
static size_t wh_char(const char *s, size_t n) {
    unsigned char c = (unsigned char)s[0];
    size_t l = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return l < n ? l : n;
}
static void wh_puts(wh_out *o, const char *s) { wh_put(o, s, strlen(s)); }

static int starts(const char *s, size_t n, const char *pfx) {
    size_t l = strlen(pfx);
    return n >= l && memcmp(s, pfx, l) == 0;
}

/* The common chat-webhook format's markup in `s`, as the message format writes
 * it (MARKDOWN.md): `<url|label>` a labelled link, `<url>` the address,
 * `<!here>` and its kind @here, `<#ID|name>` #name, and the three entities it
 * escapes. A mention by id, `<@ID>`, and anything else is left as written. */
static void wh_convert(wh_out *o, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (s[i] == '&') {
            if (starts(s + i, n - i, "&lt;"))  { wh_puts(o, "<"); i += 4; continue; }
            if (starts(s + i, n - i, "&gt;"))  { wh_puts(o, ">"); i += 4; continue; }
            if (starts(s + i, n - i, "&amp;")) { wh_puts(o, "&"); i += 5; continue; }
        }
        if (s[i] == '<') {
            size_t j = i + 1;
            while (j < n && s[j] != '>' && s[j] != '<' && s[j] != '\n') j++;
            if (j < n && s[j] == '>' && j > i + 1) {
                const char *in = s + i + 1;
                size_t il = j - i - 1;
                const char *bar = memchr(in, '|', il);
                size_t vl = bar ? (size_t)(bar - in) : il;              /* before the bar */
                const char *lab = bar ? bar + 1 : NULL;
                size_t ll = bar ? il - vl - 1 : 0;
                if (starts(in, il, "!here") || starts(in, il, "!channel") || starts(in, il, "!everyone")) {
                    wh_puts(o, "@");
                    wh_put(o, in + 1, vl - 1);
                    i = j + 1;
                    continue;
                }
                if (in[0] == '#' && lab && ll) {
                    wh_puts(o, "#");
                    wh_put(o, lab, ll);
                    i = j + 1;
                    continue;
                }
                if (starts(in, vl, "http://") || starts(in, vl, "https://")) {
                    int plain = !lab || !ll || memchr(lab, '[', ll) || memchr(lab, ']', ll);
                    if (lab && ll && !plain) {
                        wh_puts(o, "[");
                        wh_put(o, lab, ll);
                        wh_puts(o, "](");
                        wh_put(o, in, vl);
                        wh_puts(o, ")");
                    } else if (lab && ll) {                   /* a label the format cannot hold */
                        wh_put(o, lab, ll);
                        wh_puts(o, " (");
                        wh_put(o, in, vl);
                        wh_puts(o, ")");
                    } else {
                        wh_put(o, in, vl);
                    }
                    i = j + 1;
                    continue;
                }
                if (starts(in, vl, "mailto:")) {
                    /* Not an address the format links: the address, as text. */
                    if (lab && ll) wh_put(o, lab, ll);
                    else wh_put(o, in + 7, vl - 7);
                    i = j + 1;
                    continue;
                }
            }
        }
        size_t cl = wh_char(s + i, n - i);
        wh_put(o, s + i, cl);
        i += cl;
    }
}

/* One string from the payload, unescaped and converted, on a line of its own. */
static void wh_line(wh_out *o, const oc_json *d, int t) {
    if (t < 0 || t >= d->n || d->t[t].type != JSMN_STRING) return;
    size_t rl = (size_t)(d->t[t].end - d->t[t].start);
    char *u = malloc(rl + 1);
    if (!u) return;
    if (oc_json_str(d, t, u, rl + 1) == 0 && u[0]) {
        if (o->n) wh_puts(o, "\n");
        wh_convert(o, u, strlen(u));
    }
    free(u);
}

/* The string at `key` of the object `obj`'s object `sub`: `text.text`. */
static int wh_sub(const oc_json *d, int obj, const char *sub, const char *key) {
    int s = oc_json_get(d, obj, sub);
    return s >= 0 && d->t[s].type == JSMN_OBJECT ? oc_json_get(d, s, key) : -1;
}

/* Each element of the array at `arr`, by index: -1 past the last. */
static int wh_elem(const oc_json *d, int arr, int k) {
    if (arr < 0 || arr >= d->n || d->t[arr].type != JSMN_ARRAY || k >= d->t[arr].size) return -1;
    int i = arr + 1;
    for (int q = 0; q < k && i < d->n; q++) i = oc_json_skip(d, i);
    return i < d->n ? i : -1;
}

static int wh_is(const oc_json *d, int t, const char *s) {
    size_t l = strlen(s);
    return t >= 0 && d->t[t].type == JSMN_STRING && (size_t)(d->t[t].end - d->t[t].start) == l &&
           memcmp(d->js + d->t[t].start, s, l) == 0;
}

/* The blocks' text: header, section (its text and each field) and context (each
 * element's text), a block to a line; any other kind is passed over. */
static void wh_blocks(wh_out *o, const oc_json *d, int arr) {
    for (int k = 0, b; (b = wh_elem(d, arr, k)) >= 0; k++) {
        if (d->t[b].type != JSMN_OBJECT) continue;
        int ty = oc_json_get(d, b, "type");
        if (wh_is(d, ty, "header") || wh_is(d, ty, "section")) {
            wh_line(o, d, wh_sub(d, b, "text", "text"));
            if (wh_is(d, ty, "section")) {
                int fs = oc_json_get(d, b, "fields");
                for (int q = 0, f; (f = wh_elem(d, fs, q)) >= 0; q++)
                    if (d->t[f].type == JSMN_OBJECT) wh_line(o, d, oc_json_get(d, f, "text"));
            }
        } else if (wh_is(d, ty, "context")) {
            int es = oc_json_get(d, b, "elements");
            for (int q = 0, e; (e = wh_elem(d, es, q)) >= 0; q++)
                if (d->t[e].type == JSMN_OBJECT) wh_line(o, d, oc_json_get(d, e, "text"));
        }
    }
}

/* The string at `t` decoded into a malloc'd C string, or NULL. */
static char *wh_str(const oc_json *d, int t) {
    if (t < 0 || t >= d->n || d->t[t].type != JSMN_STRING) return NULL;
    size_t rl = (size_t)(d->t[t].end - d->t[t].start);
    char *u = malloc(rl + 1);
    if (u && (oc_json_str(d, t, u, rl + 1) != 0 || !u[0])) { free(u); u = NULL; }
    return u;
}

/* Each attachment: its pretext, title (a labelled link with its title_link),
 * text and fields (`*title*: value`), each on its lines; with none of those, its
 * fallback. */
static void wh_attachments(wh_out *o, const oc_json *d, int arr) {
    for (int k = 0, a; (a = wh_elem(d, arr, k)) >= 0; k++) {
        if (d->t[a].type != JSMN_OBJECT) continue;
        size_t before = o->n;
        wh_line(o, d, oc_json_get(d, a, "pretext"));
        char *title = wh_str(d, oc_json_get(d, a, "title"));
        if (title) {
            char *link = wh_str(d, oc_json_get(d, a, "title_link"));
            /* As the format writes a link, so it converts as one. */
            size_t cl = strlen(title) + (link ? strlen(link) : 0) + 4;
            char *c = malloc(cl);
            if (c) {
                if (link) snprintf(c, cl, "<%s|%s>", link, title);
                else      snprintf(c, cl, "%s", title);
                if (o->n) wh_puts(o, "\n");
                wh_convert(o, c, strlen(c));
                free(c);
            }
            free(link);
            free(title);
        }
        wh_line(o, d, oc_json_get(d, a, "text"));
        int fs = oc_json_get(d, a, "fields");
        for (int q = 0, f; (f = wh_elem(d, fs, q)) >= 0; q++) {
            if (d->t[f].type != JSMN_OBJECT) continue;
            char *ft = wh_str(d, oc_json_get(d, f, "title"));
            char *fv = wh_str(d, oc_json_get(d, f, "value"));
            if (ft || fv) {
                if (o->n) wh_puts(o, "\n");
                if (ft) { wh_puts(o, "*"); wh_convert(o, ft, strlen(ft)); wh_puts(o, "*"); }
                if (ft && fv) wh_puts(o, ": ");
                if (fv) wh_convert(o, fv, strlen(fv));
            }
            free(ft);
            free(fv);
        }
        if (o->n == before) wh_line(o, d, oc_json_get(d, a, "fallback"));
    }
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A form body's `payload` field, URL-decoded into a malloc'd string, or NULL. */
static char *form_payload(const char *b, size_t n, size_t *len) {
    size_t i = 0;
    while (i < n) {
        size_t e = i;
        while (e < n && b[e] != '&') e++;
        if (e - i >= 8 && memcmp(b + i, "payload=", 8) == 0) {
            char *out = malloc(e - i + 1);
            size_t o = 0;
            if (!out) return NULL;
            for (size_t k = i + 8; k < e; k++) {
                if (b[k] == '+') out[o++] = ' ';
                else if (b[k] == '%' && k + 2 < e && hexv(b[k + 1]) >= 0 && hexv(b[k + 2]) >= 0) {
                    out[o++] = (char)(hexv(b[k + 1]) * 16 + hexv(b[k + 2]));
                    k += 2;
                } else out[o++] = b[k];
            }
            out[o] = '\0';
            *len = o;
            return out;
        }
        i = e + 1;
    }
    return NULL;
}

int oc_http_webhook_payload(const oc_http_req *req, char *out, size_t cap, size_t *outlen) {
    *outlen = 0;
    if (!out || cap < 2) return 0;
    out[0] = '\0';
    if (!req->body || req->body_len == 0) return 0;
    if (!req->is_json && !req->is_form) {
        /* Plain text is posted as sent, as far as it fits. */
        wh_out o = { out, 0, cap, 0 };
        wh_put(&o, req->body, req->body_len);
        *outlen = o.n;
        return o.n > 0;
    }
    const char *js = req->body;
    size_t jl = req->body_len;
    char *form = NULL;
    if (req->is_form) {
        form = form_payload(req->body, req->body_len, &jl);
        if (!form) return 0;
        js = form;
    }
    oc_json d;
    /* Past the reader's token limit the payload is refused, never cut short. */
    if (oc_json_parse(&d, js, jl) != 0) { free(form); return 0; }
    wh_out o = { out, 0, cap, 0 };
    if (d.n > 0 && d.t[0].type == JSMN_OBJECT) {
        /* Blocks win over text, which that format sends as the notification's
         * fallback; then the attachments. `username`, the icons, `channel`,
         * `mrkdwn` and the unfurl flags are read past: the webhook's label is
         * its author (ARCH-71). */
        int blocks = oc_json_get(&d, 0, "blocks");
        if (blocks >= 0 && d.t[blocks].type == JSMN_ARRAY && d.t[blocks].size > 0) wh_blocks(&o, &d, blocks);
        else wh_line(&o, &d, oc_json_get(&d, 0, "text"));
        wh_attachments(&o, &d, oc_json_get(&d, 0, "attachments"));
    }
    oc_json_free(&d);
    free(form);
    *outlen = o.n;
    return o.n > 0;
}

/* --- routes ------------------------------------------------------------------ */

static int path_matches(const oc_http_route *rt, const char *p, size_t plen) {
    size_t n = strlen(rt->path);
    if (rt->prefix) return plen > n && memcmp(p, rt->path, n) == 0;
    return plen == n && memcmp(p, rt->path, n) == 0;
}

const oc_http_route *oc_http_route_find(const oc_http_site *site, const oc_http_req *req,
                                        int *status) {
    const char *q = memchr(req->path, '?', req->path_len);
    size_t plen = q ? (size_t)(q - req->path) : req->path_len;
    int named = 0;
    for (size_t i = 0; site && i < site->n; i++) {
        const oc_http_route *rt = &site->routes[i];
        if (!path_matches(rt, req->path, plen)) continue;
        named = 1;
        if (!rt->method || (strlen(rt->method) == req->method_len &&
                            memcmp(rt->method, req->method, req->method_len) == 0))
            return rt;
    }
    if (named) { *status = 405; return NULL; }
    if (site && site->fallback) return site->fallback;
    *status = 404;
    return NULL;
}

/* --- responses --------------------------------------------------------------- */

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 302: return "Found";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

size_t oc_http_head(char *out, size_t cap, int status, const char *ctype, size_t body_len) {
    return oc_http_head_ex(out, cap, status, ctype, body_len, NULL);
}

size_t oc_http_head_ex(char *out, size_t cap, int status, const char *ctype, size_t body_len,
                       const char *extra) {
    int n = snprintf(out, cap,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "%s"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status, reason(status), ctype, extra ? extra : "", body_len);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

/* One comma-separated element of a header list, its surrounding blanks
 * trimmed: [*s, *s + *n). Advances *pos past it; 0 when there are no more. */
static int list_next(const char *v, size_t n, size_t *pos, const char **s, size_t *len) {
    size_t i = *pos;
    while (i < n && (v[i] == ' ' || v[i] == '\t' || v[i] == ',')) i++;
    if (i >= n) return 0;
    size_t b = i;
    while (i < n && v[i] != ',') i++;
    size_t e = i;
    while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t')) e--;
    *s = v + b; *len = e - b; *pos = i;
    return 1;
}

int oc_http_accepts_gzip(const char *v, size_t n) {
    const char *s; size_t len, pos = 0;
    int star = 0;
    while (list_next(v, n, &pos, &s, &len)) {
        size_t k = 0;
        while (k < len && s[k] != ';' && s[k] != ' ' && s[k] != '\t') k++;
        /* "q=0" (or 0.0, 0.00...) refuses; any other weight takes it. */
        int refused = 0;
        for (size_t j = k; j + 1 < len; j++)
            if ((s[j] == 'q' || s[j] == 'Q') && s[j + 1] == '=') {
                size_t d = j + 2;
                refused = d < len && s[d] == '0';
                for (d++; refused && d < len && s[d] != ';' && s[d] != ' '; d++)
                    if (s[d] != '.' && s[d] != '0') refused = 0;
                break;
            }
        if (ci_eq(s, k, "gzip") || ci_eq(s, k, "x-gzip")) return !refused;
        if (k == 1 && s[0] == '*') star = !refused;
    }
    return star;
}

int oc_http_etag_listed(const char *v, size_t n, const char *etag) {
    if (!v || !etag) return 0;
    size_t el = strlen(etag);
    const char *s; size_t len, pos = 0;
    while (list_next(v, n, &pos, &s, &len)) {
        if (len == 1 && s[0] == '*') return 1;
        if (len > 2 && s[0] == 'W' && s[1] == '/') { s += 2; len -= 2; }
        if (len == el && memcmp(s, etag, el) == 0) return 1;
    }
    return 0;
}

void oc_http_static_choose(const oc_http_route *rt, const oc_http_req *req, oc_http_static_pick *out) {
    int gz = rt->gz_body && rt->gz_len && req->accept_gzip;
    out->gzip = gz;
    out->etag = gz ? rt->etag_gz : rt->etag;
    if (out->etag && req->inm && oc_http_etag_listed(req->inm, req->inm_len, out->etag)) {
        out->status = 304; out->body = ""; out->len = 0;
        return;
    }
    out->status = 200;
    out->body = gz ? rt->gz_body : rt->body;
    out->len  = gz ? rt->gz_len  : rt->body_len;
}

size_t oc_http_static_head(char *out, size_t cap, const oc_http_route *rt, const oc_http_static_pick *p) {
    char more[512];
    int m = snprintf(more, sizeof more, "%s%s%s%s%s%s",
                     rt->extra ? rt->extra : "",
                     p->etag ? "ETag: " : "", p->etag ? p->etag : "", p->etag ? "\r\n" : "",
                     p->gzip ? "Content-Encoding: gzip\r\n" : "",
                     rt->gz_body ? "Vary: Accept-Encoding\r\n" : "");
    if (m < 0 || (size_t)m >= sizeof more) return 0;
    if (p->status != 304) return oc_http_head_ex(out, cap, p->status, rt->ctype, p->len, more);
    int n = snprintf(out, cap, "HTTP/1.1 304 Not Modified\r\n%sConnection: close\r\n\r\n", more);
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

const char *oc_http_error_body(int status, size_t *len) {
    const char *b;
    switch (status) {
    case 400: b = "bad request\n"; break;
    case 404: b = "not found\n"; break;
    case 405: b = "method not allowed\n"; break;
    case 408: b = "request timeout\n"; break;
    case 413: b = "too large\n"; break;
    case 429: b = "rate limited\n"; break;
    default:  b = "error\n"; break;
    }
    *len = strlen(b);
    return b;
}

int oc_ws_parse(const uint8_t *buf, size_t n, uint64_t max_len, oc_ws_frame *out) {
    if (n < 2) return OC_WS_MORE;
    uint8_t b0 = buf[0], b1 = buf[1];
    if (b0 & 0x70) return OC_WS_REFUSE;          /* reserved bits: no extension agreed */
    if (!(b1 & 0x80)) return OC_WS_REFUSE;       /* a client's frames are masked */
    uint64_t len = b1 & 0x7f;
    size_t hdr = 2;
    if (len == 126) {
        if (n < 4) return OC_WS_MORE;
        len = ((uint64_t)buf[2] << 8) | buf[3];
        hdr = 4;
    } else if (len == 127) {
        if (n < 10) return OC_WS_MORE;
        len = 0;
        for (int i = 0; i < 8; i++) len = (len << 8) | buf[2 + i];
        hdr = 10;
    }
    if (len > max_len) return OC_WS_REFUSE;
    size_t whole = hdr + 4 + (size_t)len;
    if (n < whole) return OC_WS_MORE;
    out->opcode = b0 & 0x0f;
    out->fin = (b0 & 0x80) != 0;
    out->mask_off = hdr;
    out->payload_off = hdr + 4;
    out->len = (size_t)len;
    out->whole = whole;
    return OC_WS_FRAME;
}
