/* The daemon as the MCP endpoint's authorization server, its pure parts: see
 * oauth.h and docs/details/AUTH.md §8.13. */

#include "oauth.h"
#include "json.h"
#include "jwt.h"    /* oc_base64url_encode */
#include "auth.h"   /* oc_rand_bytes, oc_sha256 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The device codes' alphabet (daemon/devicecodes.c): consonants only, so a
 * code spells no word and no letter is mistaken for a digit. */
static const char ALPHA[] = "BCDFGHJKLMNPQRSTVWXZ";

int oc_oauth_base(const char *host, size_t len, char *out, size_t cap) {
    if (!host || !len || len > 253) return -1;
    for (size_t i = 0; i < len; i++) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == ':' || c == '[' || c == ']'))
            return -1;
    }
    int n = snprintf(out, cap, "https://%.*s", (int)len, host);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

void oc_oauth_resource_metadata(oc_mcp_buf *b, const char *base) {
    oc_mcp_putf(b, "{\"resource\":\"%s/mcp\",\"authorization_servers\":[\"%s\"],"
                   "\"bearer_methods_supported\":[\"header\"],\"resource_name\":\"OpenChime\"}", base, base);
}

void oc_oauth_server_metadata(oc_mcp_buf *b, const char *base) {
    oc_mcp_putf(b, "{\"issuer\":\"%s\",\"authorization_endpoint\":\"%s" OC_OAUTH_AUTHORIZE_PATH "\","
                   "\"token_endpoint\":\"%s" OC_OAUTH_TOKEN_PATH "\",\"registration_endpoint\":\"%s" OC_OAUTH_REGISTER_PATH "\","
                   "\"response_types_supported\":[\"code\"],"
                   "\"grant_types_supported\":[\"authorization_code\",\"refresh_token\"],"
                   "\"code_challenge_methods_supported\":[\"S256\"],"
                   "\"token_endpoint_auth_methods_supported\":[\"none\"],"
                   "\"authorization_response_iss_parameter_supported\":true,"
                   "\"client_id_metadata_document_supported\":true}", base, base, base, base);
}

static int plain_chars(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p <= 0x20 || *p == 0x7F || *p == '#' || *p == '"' || *p == '<' || *p == '>' || *p == '\\') return 0;
    return 1;
}

/* After `scheme`, the authority's end (the first '/', '?' or the end). */
static const char *authority_end(const char *a) {
    while (*a && *a != '/' && *a != '?') a++;
    return a;
}

/* The loopback host at `a` (127.0.0.1, [::1] or localhost), followed by a
 * port, a path, a query or the end: the length of the host, or 0. */
static size_t loopback_host(const char *a) {
    static const char *const HOSTS[] = { "127.0.0.1", "[::1]", "localhost" };
    for (size_t i = 0; i < 3; i++) {
        size_t n = strlen(HOSTS[i]);
        if (strncmp(a, HOSTS[i], n) == 0 && (a[n] == ':' || a[n] == '/' || a[n] == '?' || a[n] == '\0')) return n;
    }
    return 0;
}

int oc_oauth_redirect_ok(const char *uri) {
    if (!uri || !*uri || strlen(uri) > OC_OAUTH_URI_MAX || !plain_chars(uri)) return 0;
    if (strncmp(uri, "https://", 8) == 0) {
        const char *a = uri + 8;
        return authority_end(a) > a && a[0] != ':' && a[0] != '@' && !strchr(a, '@');
    }
    if (strncmp(uri, "http://", 7) == 0) return loopback_host(uri + 7) != 0;
    return 0;
}

/* A loopback http URI without its port: the scheme and host, then the rest. */
static int loopback_parts(const char *u, size_t *host_end, const char **rest) {
    if (strncmp(u, "http://", 7) != 0) return 0;
    size_t h = loopback_host(u + 7);
    if (!h) return 0;
    const char *p = u + 7 + h;
    if (*p == ':') { p++; while (*p >= '0' && *p <= '9') p++; }
    *host_end = 7 + h;
    *rest = p;
    return 1;
}

static int same_uri(const char *declared, size_t dlen, const char *uri) {
    if (strlen(uri) == dlen && memcmp(declared, uri, dlen) == 0) return 1;
    char d[OC_OAUTH_URI_MAX + 1];
    if (dlen > OC_OAUTH_URI_MAX) return 0;
    memcpy(d, declared, dlen);
    d[dlen] = '\0';
    size_t dh, uh;
    const char *dr, *ur;
    return loopback_parts(d, &dh, &dr) && loopback_parts(uri, &uh, &ur) && dh == uh && memcmp(d, uri, dh) == 0 &&
           strcmp(dr, ur) == 0;
}

int oc_oauth_uris_has(const char *lines, const char *uri) {
    if (!lines || !uri || !*uri) return 0;
    for (const char *p = lines; *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && same_uri(p, n, uri)) return 1;
        p = e ? e + 1 : p + n;
    }
    return 0;
}

int oc_oauth_client_id_is_url(const char *id, int allow_http_loopback) {
    if (!id || strlen(id) > OC_OAUTH_URI_MAX || !plain_chars(id) || strchr(id, '?')) return 0;
    const char *a;
    if (strncmp(id, "https://", 8) == 0) a = id + 8;
    else if (allow_http_loopback && strncmp(id, "http://127.0.0.1", 16) == 0) a = id + 7;
    else return 0;
    if (strchr(a, '@')) return 0;
    const char *e = authority_end(a);
    return e > a && e[0] == '/' && e[1] != '\0';
}

void oc_oauth_uris_join(const oc_oauth_client_meta *m, char *out, size_t cap) {
    size_t o = 0;
    if (cap) out[0] = '\0';
    for (size_t i = 0; i < m->n_uris; i++) {
        int n = snprintf(out + o, cap - o, "%s%s", i ? "\n" : "", m->uris[i]);
        if (n < 0 || (size_t)n >= cap - o) return;
        o += (size_t)n;
    }
}

static void say(char *err, size_t cap, const char *what) {
    if (err && cap) snprintf(err, cap, "%s", what);
}

/* Each string of the array at `i` must be one of `allowed` (NULL-ended). */
static int strings_within(const oc_json *d, int i, const char *const *allowed) {
    if (i < 0) return 1;
    if (d->t[i].type != JSMN_ARRAY) return 0;
    int k = i + 1;
    for (int n = 0; n < d->t[i].size; n++) {
        char v[64];
        if (oc_json_str(d, k, v, sizeof v) != 0) return 0;
        int ok = 0;
        for (size_t a = 0; allowed[a]; a++) if (strcmp(v, allowed[a]) == 0) ok = 1;
        if (!ok) return 0;
        k = oc_json_skip(d, k);
    }
    return 1;
}

static int name_ok(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7F) return 0;
    return 1;
}

static int read_meta(const oc_json *d, oc_oauth_client_meta *out, char *err, size_t errcap) {
    memset(out, 0, sizeof *out);
    if (d->n < 1 || d->t[0].type != JSMN_OBJECT) { say(err, errcap, "the body is not a JSON object"); return -1; }
    int nm = oc_json_get(d, 0, "client_name");
    if (nm >= 0) {
        if (oc_json_str(d, nm, out->name, sizeof out->name) != 0 || !name_ok(out->name)) {
            say(err, errcap, "client_name is not a name of at most 100 bytes");
            return -1;
        }
    }
    if (!out->name[0]) snprintf(out->name, sizeof out->name, "%s", "An unnamed client");
    int ru = oc_json_get(d, 0, "redirect_uris");
    if (ru < 0 || d->t[ru].type != JSMN_ARRAY || d->t[ru].size < 1 || d->t[ru].size > OC_OAUTH_URIS_MAX) {
        say(err, errcap, "redirect_uris must list one to eight URIs");
        return -1;
    }
    int k = ru + 1;
    for (int n = 0; n < d->t[ru].size; n++) {
        if (oc_json_str(d, k, out->uris[n], sizeof out->uris[n]) != 0 || !oc_oauth_redirect_ok(out->uris[n])) {
            say(err, errcap, "a redirect URI must be https, or http to a loopback address");
            return -1;
        }
        out->n_uris++;
        k = oc_json_skip(d, k);
    }
    char am[32] = "";
    int ta = oc_json_get(d, 0, "token_endpoint_auth_method");
    if (ta >= 0 && (oc_json_str(d, ta, am, sizeof am) != 0 || strcmp(am, "none") != 0)) {
        say(err, errcap, "only public clients are registered: token_endpoint_auth_method must be none");
        return -1;
    }
    static const char *const GRANTS[] = { "authorization_code", "refresh_token", NULL };
    static const char *const RESPONSES[] = { "code", NULL };
    if (!strings_within(d, oc_json_get(d, 0, "grant_types"), GRANTS)) {
        say(err, errcap, "grant_types may hold only authorization_code and refresh_token");
        return -1;
    }
    if (!strings_within(d, oc_json_get(d, 0, "response_types"), RESPONSES)) {
        say(err, errcap, "response_types may hold only code");
        return -1;
    }
    return 0;
}

int oc_oauth_parse_registration(const char *body, size_t len, oc_oauth_client_meta *out, char *err, size_t errcap) {
    oc_json d;
    if (!body || oc_json_parse(&d, body, len) != 0) { memset(out, 0, sizeof *out); say(err, errcap, "the body is not JSON"); return -1; }
    int rc = read_meta(&d, out, err, errcap);
    oc_json_free(&d);
    return rc;
}

int oc_oauth_parse_document(const char *body, size_t len, const char *url, int allow_http_loopback,
                            oc_oauth_client_meta *out, char *err, size_t errcap) {
    memset(out, 0, sizeof *out);
    if (!url || !oc_oauth_client_id_is_url(url, allow_http_loopback)) { say(err, errcap, "the client id is not a document's URL"); return -1; }
    if (len > OC_OAUTH_DOC_MAX) { say(err, errcap, "the client's metadata document is over 64 KiB"); return -1; }
    oc_json d;
    if (!body || oc_json_parse(&d, body, len) != 0) { say(err, errcap, "the client's metadata document is not JSON"); return -1; }
    int rc = read_meta(&d, out, err, errcap);
    char id[OC_OAUTH_URI_MAX + 1];
    if (rc == 0 && (d.t[0].type != JSMN_OBJECT || oc_json_get_str(&d, 0, "client_id", id, sizeof id) != 0 || strcmp(id, url) != 0)) {
        say(err, errcap, "the client's metadata document names another client_id");
        rc = -1;
    }
    oc_json_free(&d);
    return rc;
}

void oc_oauth_registered(oc_mcp_buf *b, const char *client_id, const oc_oauth_client_meta *m, uint64_t issued_s) {
    oc_mcp_put(b, "{\"client_id\":");
    oc_mcp_put_str(b, client_id, strlen(client_id));
    oc_mcp_putf(b, ",\"client_id_issued_at\":%llu,\"client_name\":", (unsigned long long)issued_s);
    oc_mcp_put_str(b, m->name, strlen(m->name));
    oc_mcp_put(b, ",\"redirect_uris\":[");
    for (size_t i = 0; i < m->n_uris; i++) {
        if (i) oc_mcp_put(b, ",");
        oc_mcp_put_str(b, m->uris[i], strlen(m->uris[i]));
    }
    oc_mcp_put(b, "],\"grant_types\":[\"authorization_code\",\"refresh_token\"],\"response_types\":[\"code\"],"
                  "\"token_endpoint_auth_method\":\"none\"}");
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int oc_oauth_form_get(const char *form, size_t len, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    int found = 0;
    for (size_t i = 0; i < len;) {
        size_t e = i;
        while (e < len && form[e] != '&') e++;
        if (e - i > kl && memcmp(form + i, key, kl) == 0 && form[i + kl] == '=') {
            if (found++) return -2;
            size_t o = 0;
            for (size_t k = i + kl + 1; k < e; k++) {
                char c = form[k];
                if (c == '+') c = ' ';
                else if (c == '%') {
                    if (k + 2 >= e) return -2;
                    int h = hexval(form[k + 1]), l = hexval(form[k + 2]);
                    if (h < 0 || l < 0) return -2;
                    c = (char)(h * 16 + l);
                    k += 2;
                }
                if (c == '\0' || o + 1 >= cap) return -2;
                out[o++] = c;
            }
            out[o] = '\0';
        } else if (e - i == kl && memcmp(form + i, key, kl) == 0) {
            if (found++) return -2;
            if (!cap) return -2;
            out[0] = '\0';
        }
        i = e + 1;
    }
    return found ? 0 : -1;
}

static int b64url_only(const char *s, size_t n) {
    if (strlen(s) != n) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

int oc_oauth_parse_authorize(const char *query, size_t len, const char *resource, oc_oauth_authorize *out,
                             char *err, size_t errcap) {
    memset(out, 0, sizeof *out);
    int rq = oc_oauth_form_get(query, len, "request", out->request, sizeof out->request);
    if (rq == 0) {
        char typed[16];
        if (strncmp(out->request, OC_OAUTH_REQUEST_PREFIX, sizeof OC_OAUTH_REQUEST_PREFIX - 1) != 0 ||
            oc_oauth_form_get(query, len, "code", typed, sizeof typed) != 0 || oc_oauth_code_normalize(typed, out->code) != 0) {
            say(err, errcap, "This request is not one this workspace made.");
            return -1;
        }
        return 0;
    }
    char rt[16] = "", method[16] = "";
    if (oc_oauth_form_get(query, len, "response_type", rt, sizeof rt) != 0 || strcmp(rt, "code") != 0) {
        say(err, errcap, "The client asked for something other than an authorization code.");
        return -1;
    }
    if (oc_oauth_form_get(query, len, "client_id", out->client_id, sizeof out->client_id) != 0 || !out->client_id[0]) {
        say(err, errcap, "The client did not say who it is.");
        return -1;
    }
    if (oc_oauth_form_get(query, len, "redirect_uri", out->redirect_uri, sizeof out->redirect_uri) != 0 ||
        !oc_oauth_redirect_ok(out->redirect_uri)) {
        say(err, errcap, "The client did not give an address to return to that this workspace accepts.");
        return -1;
    }
    if (oc_oauth_form_get(query, len, "code_challenge_method", method, sizeof method) != 0 || strcmp(method, "S256") != 0 ||
        oc_oauth_form_get(query, len, "code_challenge", out->challenge, sizeof out->challenge) != 0 ||
        !b64url_only(out->challenge, 43)) {
        say(err, errcap, "The client did not send a PKCE S256 challenge.");
        return -1;
    }
    if (oc_oauth_form_get(query, len, "state", out->state, sizeof out->state) != 0 || !out->state[0] || !name_ok(out->state)) {
        say(err, errcap, "The client did not send a state.");
        return -1;
    }
    if (oc_oauth_form_get(query, len, "resource", out->resource, sizeof out->resource) != 0 || strcmp(out->resource, resource) != 0) {
        say(err, errcap, "The client asked for access to something other than this workspace's MCP endpoint.");
        return -1;
    }
    return 0;
}

const char *oc_oauth_parse_token(const char *form, size_t len, oc_oauth_token_req *out) {
    memset(out, 0, sizeof *out);
    char gt[32] = "";
    if (!form || oc_oauth_form_get(form, len, "grant_type", gt, sizeof gt) != 0) return "invalid_request";
    if (oc_oauth_form_get(form, len, "client_id", out->client_id, sizeof out->client_id) != 0 || !out->client_id[0])
        return "invalid_request";
    int rs = oc_oauth_form_get(form, len, "resource", out->resource, sizeof out->resource);
    if (rs == -2) return "invalid_request";
    if (strcmp(gt, "authorization_code") == 0) {
        out->grant = OC_OAUTH_GRANT_CODE;
        if (oc_oauth_form_get(form, len, "code", out->code, sizeof out->code) != 0 ||
            oc_oauth_form_get(form, len, "code_verifier", out->verifier, sizeof out->verifier) != 0 ||
            oc_oauth_form_get(form, len, "redirect_uri", out->redirect_uri, sizeof out->redirect_uri) != 0)
            return "invalid_request";
        return NULL;
    }
    if (strcmp(gt, "refresh_token") == 0) {
        out->grant = OC_OAUTH_GRANT_REFRESH;
        if (oc_oauth_form_get(form, len, "refresh_token", out->refresh, sizeof out->refresh) != 0) return "invalid_request";
        return NULL;
    }
    return "unsupported_grant_type";
}

int oc_oauth_pkce_ok(const char *verifier, const char *challenge) {
    size_t n = verifier ? strlen(verifier) : 0;
    if (n < 43 || n > 128 || !challenge) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = verifier[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
              c == '_' || c == '~'))
            return 0;
    }
    uint8_t h[32];
    char enc[48];
    if (oc_sha256(verifier, n, h) != 0) return 0;
    size_t el = oc_base64url_encode(h, sizeof h, enc);
    return strlen(challenge) == el && memcmp(enc, challenge, el) == 0;
}

int oc_oauth_mint(const char *prefix, char *out, size_t cap) {
    uint8_t raw[32];
    char enc[48];
    if (oc_rand_bytes(raw, sizeof raw) != 0) return -1;
    oc_base64url_encode(raw, sizeof raw, enc);
    int n = snprintf(out, cap, "%s%s", prefix, enc);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int oc_oauth_hash(const char *secret, size_t len, uint8_t out[32]) { return oc_sha256(secret, len, out); }

oc_oauth_token_kind oc_oauth_token_of(const char *tok, size_t len) {
    if (!tok || len < 6) return OC_OAUTH_TOKEN_OTHER;
    if (memcmp(tok, "ocpt_", 5) == 0) return OC_OAUTH_TOKEN_AGENT;
    if (memcmp(tok, OC_OAUTH_ACCESS_PREFIX, 5) == 0) return OC_OAUTH_TOKEN_PERSON;
    return OC_OAUTH_TOKEN_OTHER;
}

int oc_oauth_approval_code(char out[OC_OAUTH_CODE_LEN + 1]) {
    uint8_t r[OC_OAUTH_CODE_LEN];
    if (oc_rand_bytes(r, sizeof r) != 0) return -1;
    for (int i = 0; i < OC_OAUTH_CODE_LEN; i++) out[i] = ALPHA[r[i] % 20];
    out[OC_OAUTH_CODE_LEN] = '\0';
    return 0;
}

int oc_oauth_code_normalize(const char *in, char out[OC_OAUTH_CODE_LEN + 1]) {
    int n = 0;
    for (const char *p = in ? in : ""; *p; p++) {
        char c = *p;
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (n == OC_OAUTH_CODE_LEN || !strchr(ALPHA, c)) return -1;
        out[n++] = c;
    }
    if (n != OC_OAUTH_CODE_LEN) return -1;
    out[n] = '\0';
    return 0;
}

int oc_oauth_lifetime_ok(uint32_t days) { return days == 0 || days == 30 || days == 90 || days == 365; }

static void put_html(oc_mcp_buf *b, const char *s) {
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
        case '<': oc_mcp_put(b, "&lt;"); break;
        case '>': oc_mcp_put(b, "&gt;"); break;
        case '&': oc_mcp_put(b, "&amp;"); break;
        case '"': oc_mcp_put(b, "&quot;"); break;
        case '\'': oc_mcp_put(b, "&#39;"); break;
        default: oc_mcp_putn(b, p, 1);
        }
    }
}

void oc_oauth_page(oc_mcp_buf *b, oc_oauth_page_kind kind, const char *client_name, const char *code,
                   const char *reload, const char *message) {
    oc_mcp_put(b, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n");
    if (kind == OC_OAUTH_PAGE_PENDING && reload) {
        oc_mcp_putf(b, "<meta http-equiv=\"refresh\" content=\"%u;url=", OC_OAUTH_PAGE_REFRESH_S);
        put_html(b, reload);
        oc_mcp_put(b, "\">\n");
    }
    oc_mcp_put(b, "<title>Connect an AI client - OpenChime</title>\n<style>\n"
                  "body{background:#14161a;color:#d8dee9;font:16px/1.6 ui-sans-serif,system-ui,sans-serif;"
                  "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}\n"
                  "main{max-width:34rem;padding:2rem;text-align:center}\nh1{font-size:1.4rem;font-weight:600}\n"
                  "p{color:#9aa5b1}\n.code{font:600 2rem/1.4 ui-monospace,monospace;letter-spacing:.2em;color:#eceff4}\n"
                  "</style>\n</head>\n<body>\n<main>\n");
    switch (kind) {
    case OC_OAUTH_PAGE_PENDING:
        oc_mcp_put(b, "<h1>Connect ");
        put_html(b, client_name);
        oc_mcp_put(b, " to your workspace</h1>\n<p>Open OpenChime, signed in as yourself, choose "
                      "<strong>Approve an AI client</strong> and enter this code:</p>\n<p class=\"code\">");
        if (code && strlen(code) == OC_OAUTH_CODE_LEN) {
            oc_mcp_putn(b, code, 4);
            oc_mcp_put(b, "-");
            oc_mcp_putn(b, code + 4, 4);
        }
        oc_mcp_put(b, "</p>\n<p>This page goes on by itself once you approve or decline. The code lasts ten minutes.</p>\n");
        break;
    case OC_OAUTH_PAGE_EXPIRED:
        oc_mcp_put(b, "<h1>This request has expired</h1>\n<p>Nobody approved it within ten minutes. Start again from the AI client.</p>\n");
        break;
    case OC_OAUTH_PAGE_OFF:
        oc_mcp_put(b, "<h1>AI clients are off</h1>\n<p>This workspace does not let people connect their own AI clients.</p>\n");
        break;
    default:
        oc_mcp_put(b, "<h1>This request cannot go on</h1>\n<p>");
        put_html(b, message);
        oc_mcp_put(b, "</p>\n");
        break;
    }
    oc_mcp_put(b, "</main>\n</body>\n</html>\n");
}

static void put_pct(oc_mcp_buf *b, const char *s) {
    static const char HEX[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' ||
            *p == '_' || *p == '~') {
            oc_mcp_putn(b, (const char *)p, 1);
        } else {
            char e[3] = { '%', HEX[*p >> 4], HEX[*p & 15] };
            oc_mcp_putn(b, e, 3);
        }
    }
}

void oc_oauth_redirect(oc_mcp_buf *b, const char *redirect_uri, const char *code, const char *error,
                       const char *state, const char *iss) {
    oc_mcp_put(b, redirect_uri);
    oc_mcp_put(b, strchr(redirect_uri, '?') ? "&" : "?");
    if (code) { oc_mcp_put(b, "code="); put_pct(b, code); }
    else { oc_mcp_put(b, "error="); put_pct(b, error ? error : "access_denied"); }
    oc_mcp_put(b, "&state=");
    put_pct(b, state);
    if (iss) { oc_mcp_put(b, "&iss="); put_pct(b, iss); }
}

void oc_oauth_token_json(oc_mcp_buf *b, const char *access, const char *refresh, unsigned expires_in) {
    oc_mcp_putf(b, "{\"access_token\":\"%s\",\"token_type\":\"Bearer\",\"expires_in\":%u,\"refresh_token\":\"%s\"}",
                access, expires_in, refresh);
}

void oc_oauth_error_json(oc_mcp_buf *b, const char *error, const char *description) {
    oc_mcp_putf(b, "{\"error\":\"%s\"", error);
    if (description) { oc_mcp_put(b, ",\"error_description\":"); oc_mcp_put_str(b, description, strlen(description)); }
    oc_mcp_put(b, "}");
}
