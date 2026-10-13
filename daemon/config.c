/* The configuration loader (config.h): the registry (config_table.c) read
 * strictly into oc_config, the environment checked for names the registry
 * does not know, and the views the command line prints from the same table. */

#include "config.h"
#include "config_table.h"
#include "proxyproto.h"
#include "protocol.h"
#include "tls.h"        /* oc_tls_set_extra_ca, oc_tls_set_os_roots */
#include "url.h"        /* oc_url_transport_ok */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static oc_config g_cfg;
/* The structured rows' pieces, pointed at from g_cfg; one load at a time. */
static char g_tls_names[512], g_tls_dir[256], g_tls_email[128];
static char g_s3_buf[1024];
static char g_sum_url[512], g_sum_model[128];

/* Where each row's value came from, for --check-config: one slot per row, or
 * per instance of a pattern row. */
enum { SRC_DEFAULT = 0, SRC_ENV = 1, SRC_FILE = 2 };
#define MAX_SLOTS 160
static unsigned char g_src[MAX_SLOTS];
/* The names an EnvironmentFile set (oc_config_apply_env_file), so the report
 * can tell "file" from "environment". */
static char  g_file_names[64][64];
static int   g_n_file_names;

const char *oc_mode_name(oc_mode m) {
    switch (m) {
        case OC_MODE_DEVELOPMENT: return "development";
        case OC_MODE_DEMO:        return "demo";
        default:                  return "normal";
    }
}

const char *oc_deploy_mode_name(oc_deploy_mode m) {
    switch (m) {
        case OC_DEPLOY_FEDERATED: return "federated";
        case OC_DEPLOY_MANAGED:   return "managed";
        default:                  return "standalone";
    }
}

const oc_config *oc_config_get(void) { return &g_cfg; }

/* --- strict parsers ---------------------------------------------------------- */

static int fail(char *err, size_t errcap, const char *name, const char *v, const char *want) {
    snprintf(err, errcap, "%s=%s: %s", name, v, want);
    return -1;
}

static int parse_i64(const char *v, int64_t *out) {
    if (!*v) return -1;
    const char *p = v;
    if (*p == '-') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    for (const char *q = p; *q; q++) if (!isdigit((unsigned char)*q)) return -1;
    errno = 0;
    char *end = NULL;
    long long n = strtoll(v, &end, 10);
    if (errno || !end || *end) return -1;
    *out = n;
    return 0;
}

static int parse_bool(const char *v, int *out) {
    static const char *const yes[] = { "1", "on", "true", "yes", NULL };
    static const char *const no[]  = { "0", "off", "false", "no", NULL };
    for (int i = 0; yes[i]; i++) if (!strcmp(v, yes[i])) { *out = 1; return 0; }
    for (int i = 0; no[i]; i++)  if (!strcmp(v, no[i]))  { *out = 0; return 0; }
    return -1;
}

static int parse_enum(const char *enums, const char *v, int *out) {
    int idx = 0;
    for (const char *p = enums; p; idx++) {
        const char *e = strchr(p, '|');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (strlen(v) == n && !strncmp(p, v, n)) { *out = idx; return 0; }
        p = e ? e + 1 : NULL;
    }
    return -1;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

/* --- the structured rows ----------------------------------------------------- */

int cfg_parse_trusted_proxies(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap) {
    (void)r;
    /* A list that cannot be read stops the boot: read wrongly it is either
     * "every client is one address" or "every connection is refused". */
    char why[160];
    oc_trusted_proxies *t = oc_trusted_proxies_parse(v, why, sizeof why);
    if (!t) return fail(err, errcap, name, v, why);
    oc_trusted_proxies_free(t);
    c->trusted_proxies = v;
    return 0;
}

int cfg_parse_token_prefix(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap) {
    (void)r;
    size_t n = strlen(v);
    if (n % 2 || n / 2 > sizeof c->audio_token_prefix)
        return fail(err, errcap, name, v, "must be an even number of hex digits, at most 32");
    for (size_t i = 0; i < n; i += 2) {
        unsigned b;
        if (!isxdigit((unsigned char)v[i]) || !isxdigit((unsigned char)v[i + 1]) || sscanf(v + i, "%2x", &b) != 1)
            return fail(err, errcap, name, v, "must be hex digits");
        c->audio_token_prefix[i / 2] = (uint8_t)b;
    }
    c->audio_token_prefix_len = n / 2;
    return 0;
}

/* A direct connection (AUTH.md §8.5). Anything not understood stops the boot:
 * a typo in how people sign in must not quietly become nobody signing in. */
int cfg_parse_connect(oc_config *c, const cfg_row *r, const char *name, const char *spec, char *err, size_t errcap) {
    (void)r;
    if (c->n_connect >= OC_OIDC_MAX_CONNECT) return fail(err, errcap, name, spec, "too many connections");
    oc_oidc_connect *k = &c->connect[c->n_connect];
    memset(k, 0, sizeof *k);
    char secret_file[512] = "";
    const char *p = spec;
    while (*p) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        if (len) {
            if (!eq) { snprintf(err, errcap, "%s: \"%.*s\" is not key=value", name, (int)len, p); return -1; }
            size_t kl = (size_t)(eq - p), vl = len - kl - 1;
            const char *v = eq + 1;
            char *dst = NULL; size_t cap = 0;
            if (kl == 5 && !strncmp(p, "label", 5)) { dst = k->label; cap = sizeof k->label; }
            else if (kl == 6 && !strncmp(p, "issuer", 6)) { dst = k->issuer; cap = sizeof k->issuer; }
            else if (kl == 9 && !strncmp(p, "client_id", 9)) { dst = k->client_id; cap = sizeof k->client_id; }
            else if (kl == 11 && !strncmp(p, "secret_file", 11)) { dst = secret_file; cap = sizeof secret_file; }
            else if (kl == 7 && !strncmp(p, "subject", 7)) {
                if (vl == 3 && !strncmp(v, "oid", 3)) k->subject_oid = 1;
                else if (!(vl == 3 && !strncmp(v, "sub", 3))) { snprintf(err, errcap, "%s: subject is sub or oid", name); return -1; }
            } else if (kl == 8 && !strncmp(p, "redirect", 8)) {
                if (vl == 9 && !strncmp(v, "localhost", 9)) k->localhost = 1;
                else if (!(vl == 9 && !strncmp(v, "127.0.0.1", 9))) { snprintf(err, errcap, "%s: redirect is 127.0.0.1 or localhost", name); return -1; }
            } else { snprintf(err, errcap, "%s: unknown key \"%.*s\"", name, (int)kl, p); return -1; }
            if (dst) {
                if (vl >= cap) { snprintf(err, errcap, "%s: %.*s is too long", name, (int)kl, p); return -1; }
                memcpy(dst, v, vl); dst[vl] = '\0';
            }
        }
        p = e ? e + 1 : p + len;
    }
    if (!k->label[0] || !k->client_id[0] || strncmp(k->issuer, "https://", 8) != 0) {
        snprintf(err, errcap, "%s needs label, an https issuer and client_id", name);
        return -1;
    }
    if (secret_file[0]) {
        char *sec = read_file(secret_file);
        if (!sec) { snprintf(err, errcap, "%s: cannot read %s", name, secret_file); return -1; }
        size_t sl = strlen(sec);
        while (sl && (sec[sl - 1] == '\n' || sec[sl - 1] == '\r')) sec[--sl] = '\0';
        if (!sl || sl >= sizeof k->secret) {
            memset(sec, 0, sl); free(sec);
            snprintf(err, errcap, "%s: the secret is empty or too long", name);
            return -1;
        }
        memcpy(k->secret, sec, sl + 1);
        memset(sec, 0, sl); free(sec);
    }
    c->n_connect++;
    return 0;
}


/* OPENCHIME_AUTH: `local`, `relay`, or both, comma-separated. A name it does
 * not know stops the boot: a typo must not quietly leave a workspace with no
 * way in, or with one its operator did not mean. */
int cfg_parse_auth(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap) {
    (void)r;
    c->auth_local = c->auth_relay = 0;
    const char *p = v;
    while (*p) {
        const char *e = strchr(p, ',');
        if (!e) e = p + strlen(p);
        const char *s = p, *t = e;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t')) t--;
        size_t n = (size_t)(t - s);
        if (n == 5 && !strncmp(s, "local", 5)) c->auth_local = 1;
        else if (n == 5 && !strncmp(s, "relay", 5)) c->auth_relay = 1;
        else if (n) { snprintf(err, errcap, "%s=%s: unknown source \"%.*s\" (local, relay)", name, v, (int)n, s); return -1; }
        p = *e ? e + 1 : e;
    }
    if (!c->auth_local && !c->auth_relay) return fail(err, errcap, name, v, "names no source (local, relay)");
    return 0;
}

/* OPENCHIME_TLS: self | file | central | acme:<names>[;email=…][;directory=…]. */
int cfg_parse_tls(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap) {
    (void)r;
    c->tls_src.names = c->tls_src.email = NULL;
    c->tls_src.directory = "https://acme-v02.api.letsencrypt.org/directory";
    if (!strcmp(v, "self"))    { c->tls_src.source = OC_TLS_SRC_SELF; return 0; }
    if (!strcmp(v, "file"))    { c->tls_src.source = OC_TLS_SRC_FILE; return 0; }
    if (!strcmp(v, "central")) { c->tls_src.source = OC_TLS_SRC_CENTRAL; return 0; }
    if (strncmp(v, "acme:", 5) != 0)
        return fail(err, errcap, name, v, "must be self, file, central or acme:<names>[;email=…][;directory=…]");
    c->tls_src.source = OC_TLS_SRC_ACME;
    const char *p = v + 5;
    const char *e = strchr(p, ';');
    size_t nl = e ? (size_t)(e - p) : strlen(p);
    if (!nl) return fail(err, errcap, name, v, "acme: needs the names to certify, comma-separated");
    if (nl >= sizeof g_tls_names) return fail(err, errcap, name, v, "the names are too long");
    for (size_t i = 0; i < nl; i++)
        if (!(isalnum((unsigned char)p[i]) || p[i] == '.' || p[i] == '-' || p[i] == ','))
            return fail(err, errcap, name, v, "the names are DNS names, comma-separated");
    memcpy(g_tls_names, p, nl); g_tls_names[nl] = '\0';
    c->tls_src.names = g_tls_names;
    p = e ? e + 1 : p + nl;
    while (*p) {
        e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        if (len) {
            if (!eq) { snprintf(err, errcap, "%s: \"%.*s\" is not key=value", name, (int)len, p); return -1; }
            size_t kl = (size_t)(eq - p), vl = len - kl - 1;
            char *dst; size_t cap;
            if (kl == 5 && !strncmp(p, "email", 5)) { dst = g_tls_email; cap = sizeof g_tls_email; }
            else if (kl == 9 && !strncmp(p, "directory", 9)) { dst = g_tls_dir; cap = sizeof g_tls_dir; }
            else { snprintf(err, errcap, "%s: unknown key \"%.*s\" (email, directory)", name, (int)kl, p); return -1; }
            if (!vl || vl >= cap) { snprintf(err, errcap, "%s: %.*s is empty or too long", name, (int)kl, p); return -1; }
            memcpy(dst, eq + 1, vl); dst[vl] = '\0';
            if (dst == g_tls_email) c->tls_src.email = g_tls_email;
            else {
                if (strncmp(g_tls_dir, "https://", 8) != 0) { snprintf(err, errcap, "%s: directory must be an https URL", name); return -1; }
                c->tls_src.directory = g_tls_dir;
            }
        }
        p = e ? e + 1 : p + len;
    }
    return 0;
}

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Percent-decode `n` bytes of `src` into `dst` (cap `cap`); the decoded length, or -1. */
static int pct_decode(const char *src, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int ch = (unsigned char)src[i];
        if (ch == '%') {
            if (i + 2 >= n) return -1;
            int h = hexval((unsigned char)src[i + 1]), l = hexval((unsigned char)src[i + 2]);
            if (h < 0 || l < 0) return -1;
            ch = h * 16 + l; i += 2;
        }
        if (o + 1 >= cap) return -1;
        dst[o++] = (char)ch;
    }
    dst[o] = '\0';
    return (int)o;
}

/* OPENCHIME_S3: s3://KEY:SECRET@host[:port]/bucket[?region=…], or http+s3://
 * for a plaintext endpoint (a fake in a test). Everything the backend needs is
 * in the one value, so there is no partial set to mistake for local disk. */
int cfg_parse_s3(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap) {
    (void)r;
    static const char want[] = "must be s3://KEY:SECRET@host[:port]/bucket[?region=…] (http+s3:// for plaintext)";
    const char *shown = "(set)";
    const char *p, *scheme;
    if (!strncmp(v, "s3://", 5)) { p = v + 5; scheme = "https://"; }
    else if (!strncmp(v, "http+s3://", 10)) { p = v + 10; scheme = "http://"; }
    else return fail(err, errcap, name, shown, want);
    const char *slash = strchr(p, '/');
    if (!slash) return fail(err, errcap, name, shown, want);
    const char *at = NULL;
    for (const char *q = p; q < slash; q++) if (*q == '@') at = q;
    if (!at) return fail(err, errcap, name, shown, "needs KEY:SECRET@ before the host");
    const char *colon = memchr(p, ':', (size_t)(at - p));
    if (!colon || colon == p || colon + 1 == at) return fail(err, errcap, name, shown, "needs KEY:SECRET@ before the host, neither empty");
    size_t hl = (size_t)(slash - (at + 1));
    if (!hl) return fail(err, errcap, name, shown, "needs a host");
    const char *bucket = slash + 1;
    const char *qm = strchr(bucket, '?');
    size_t bl = qm ? (size_t)(qm - bucket) : strlen(bucket);
    if (!bl || memchr(bucket, '/', bl)) return fail(err, errcap, name, shown, "needs /bucket, one path segment");
    const char *region = "us-east-1"; size_t rl = strlen(region);
    if (qm) {
        if (strncmp(qm + 1, "region=", 7) != 0 || !qm[8] || strchr(qm + 8, '&'))
            return fail(err, errcap, name, shown, "the only query is ?region=…");
        region = qm + 8; rl = strlen(region);
    }
    char *b = g_s3_buf; size_t cap = sizeof g_s3_buf;
    int n;
    c->s3.access_key = b;
    if ((n = pct_decode(p, (size_t)(colon - p), b, cap)) < 0) return fail(err, errcap, name, shown, "the key does not percent-decode");
    b += n + 1; cap -= (size_t)n + 1;
    c->s3.secret_key = b;
    if ((n = pct_decode(colon + 1, (size_t)(at - colon - 1), b, cap)) < 0) return fail(err, errcap, name, shown, "the secret does not percent-decode");
    b += n + 1; cap -= (size_t)n + 1;
    c->s3.endpoint = b;
    if (strlen(scheme) + hl + 1 > cap) return fail(err, errcap, name, shown, "too long");
    n = snprintf(b, cap, "%s%.*s", scheme, (int)hl, at + 1);
    b += n + 1; cap -= (size_t)n + 1;
    c->s3.bucket = b;
    if (bl + 1 > cap) return fail(err, errcap, name, shown, "too long");
    memcpy(b, bucket, bl); b[bl] = '\0';
    b += bl + 1; cap -= bl + 1;
    c->s3.region = b;
    if (rl + 1 > cap) return fail(err, errcap, name, shown, "too long");
    memcpy(b, region, rl + 1);
    return 0;
}

/* OPENCHIME_SUMMARY: off | local | cloud:<url>,<model>. */
int cfg_parse_summary(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap) {
    (void)r;
    c->summary.url = c->summary.model = "";
    if (!strcmp(v, "off"))   { c->summary.mode = OC_SUMMARY_OFF; return 0; }
    if (!strcmp(v, "local")) { c->summary.mode = OC_SUMMARY_LOCAL; return 0; }
    if (strncmp(v, "cloud:", 6) != 0) return fail(err, errcap, name, v, "must be off, local or cloud:<url>,<model>");
    const char *u = v + 6, *comma = strchr(u, ',');
    if (!comma) return fail(err, errcap, name, v, "cloud: needs <url>,<model>");
    size_t ul = (size_t)(comma - u), ml = strlen(comma + 1);
    if (strncmp(u, "https://", 8) != 0 && strncmp(u, "http://", 7) != 0)
        return fail(err, errcap, name, v, "the URL must be http(s)://");
    if (!ml) return fail(err, errcap, name, v, "cloud: needs <url>,<model>");
    if (ul >= sizeof g_sum_url || ml >= sizeof g_sum_model) return fail(err, errcap, name, v, "too long");
    memcpy(g_sum_url, u, ul); g_sum_url[ul] = '\0';
    memcpy(g_sum_model, comma + 1, ml + 1);
    c->summary.mode = OC_SUMMARY_CLOUD;
    c->summary.url = g_sum_url; c->summary.model = g_sum_model;
    return 0;
}

/* --- the table walk ------------------------------------------------------------ */

static int file_set(const char *name) {
    for (int i = 0; i < g_n_file_names; i++) if (!strcmp(g_file_names[i], name)) return 1;
    return 0;
}

/* Store one row's value (or its default) into the config. `src` records where
 * it came from. */
static int store(oc_config *c, const cfg_row *r, const char *name, const char *v, int is_default,
                 char *err, size_t errcap) {
    void *dst = (char *)c + r->off;
    switch (r->kind) {
    case CFG_STR:
        if (r->flags & CFG_CLIENT) return 0;
        *(const char **)dst = v;
        return 0;
    case CFG_PATH:
        if (v && !is_default && (r->flags & CFG_MUST_EXIST) && access(v, R_OK) != 0)
            return fail(err, errcap, name, v, "must be a readable file");
        *(const char **)dst = v;
        return 0;
    case CFG_INT: {
        int64_t n;
        if (!v) { *(int *)dst = 0; return 0; }
        if (parse_i64(v, &n) != 0 || n < r->min || n > r->max) {
            char want[96];
            snprintf(want, sizeof want, "must be an integer %lld..%lld", (long long)r->min, (long long)r->max);
            return fail(err, errcap, name, v, want);
        }
        *(int *)dst = (int)n;
        return 0;
    }
    case CFG_U64: {
        int64_t n;
        if (!v) { *(uint64_t *)dst = 0; return 0; }
        if (parse_i64(v, &n) != 0 || n < r->min || n > r->max) {
            char want[96];
            snprintf(want, sizeof want, "must be an integer %lld..%lld", (long long)r->min, (long long)r->max);
            return fail(err, errcap, name, v, want);
        }
        *(uint64_t *)dst = (uint64_t)n;
        return 0;
    }
    case CFG_BOOL: {
        int b = 0;
        if (v && parse_bool(v, &b) != 0) return fail(err, errcap, name, v, "must be 1, 0, on, off, true, false, yes or no");
        *(int *)dst = b;
        return 0;
    }
    case CFG_ENUM: {
        int idx = -1;
        if (v && parse_enum(r->enums, v, &idx) != 0) {
            char want[128];
            snprintf(want, sizeof want, "must be one of %s", r->enums);
            for (char *p = want; *p; p++) if (*p == '|') *p = ' ';
            return fail(err, errcap, name, v, want);
        }
        *(int *)dst = idx;
        return 0;
    }
    case CFG_CUSTOM:
        if (!v) return 0;
        return r->parse(c, r, name, v, err, errcap);
    }
    return 0;
}

/* The nearest row name to an unknown one, for the message. */
static const char *nearest(const char *name) {
    const char *best = NULL;
    size_t best_d = (size_t)-1, nl = strlen(name);
    for (const cfg_row *r = OC_CFG; r->name; r++) {
        if (r->flags & CFG_PATTERN) continue;
        /* A cheap distance: common prefix plus common suffix, against the lengths. */
        size_t rl = strlen(r->name), pre = 0, suf = 0;
        while (pre < nl && pre < rl && name[pre] == r->name[pre]) pre++;
        while (suf < nl - pre && suf < rl - pre && name[nl - 1 - suf] == r->name[rl - 1 - suf]) suf++;
        size_t d = (nl > rl ? nl : rl) - pre - suf;
        if (d < best_d) { best_d = d; best = r->name; }
    }
    return best_d <= 4 ? best : NULL;
}

static int row_matches(const cfg_row *r, const char *name) {
    if (!(r->flags & CFG_PATTERN)) return strcmp(r->name, name) == 0;
    const char *pct = strstr(r->name, "%d");
    size_t pre = (size_t)(pct - r->name);
    if (strncmp(name, r->name, pre) != 0) return 0;
    const char *num = name + pre;
    if (!isdigit((unsigned char)*num)) return 0;
    char *end = NULL;
    long n = strtol(num, &end, 10);
    return end && *end == '\0' && n >= 1 && n <= r->count;
}

/* Every OPENCHIME_* name in the environment is a row, and no removed name is
 * present. The first offender stops the load. */
static int scan_unknown(char *err, size_t errcap) {
    for (const cfg_removed *x = OC_CFG_REMOVED; x->name; x++) {
        const char *v = getenv(x->name);
        if (v && *v) { snprintf(err, errcap, "%s is no longer read: %s", x->name, x->instead); return -1; }
    }
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, "OPENCHIME_", 10) != 0) continue;
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        char name[128];
        size_t nl = (size_t)(eq - *e);
        if (nl >= sizeof name) nl = sizeof name - 1;
        memcpy(name, *e, nl); name[nl] = '\0';
        if (!eq[1]) continue;                       /* empty: unset */
        int known = 0;
        for (const cfg_row *r = OC_CFG; r->name && !known; r++) known = row_matches(r, name);
        if (!known) {
            const char *near = nearest(name);
            if (near) snprintf(err, errcap, "%s is not a setting (did you mean %s?)", name, near);
            else      snprintf(err, errcap, "%s is not a setting (CONFIG.md lists them)", name);
            return -1;
        }
    }
    return 0;
}

/* The rules that span rows, and the values derived from them. */
static int validate(oc_config *c, char *err, size_t errcap) {
    /* The deployment model (ARCH-76): what the box was started with says it. */
    int ticket = c->enroll.ticket && *c->enroll.ticket;
    c->deployment_mode = ticket ? OC_DEPLOY_MANAGED : c->enroll.url ? OC_DEPLOY_FEDERATED : OC_DEPLOY_STANDALONE;
    if (ticket && !c->enroll.url) {
        snprintf(err, errcap, "OPENCHIME_MANAGED_TICKET: a managed box is started with OPENCHIME_ENROLL_URL as well");
        return -1;
    }
    if (ticket && !c->oidc.audience) {
        snprintf(err, errcap, "OPENCHIME_MANAGED_TICKET: a managed box is started with OPENCHIME_MANAGED_AUDIENCE as well");
        return -1;
    }

    /* The data directory's layout. */
    size_t dl = strlen(c->data_dir);
    while (dl > 1 && c->data_dir[dl - 1] == '/') dl--;
    if (dl + 16 >= sizeof c->db_path) { snprintf(err, errcap, "OPENCHIME_DATA_DIR: too long"); return -1; }
    snprintf(c->db_path, sizeof c->db_path, "%.*s/openchime.db", (int)dl, c->data_dir);
    snprintf(c->blob_dir, sizeof c->blob_dir, "%.*s/blobs", (int)dl, c->data_dir);
    snprintf(c->tls_cert, sizeof c->tls_cert, "%.*s/cert.pem", (int)dl, c->data_dir);
    snprintf(c->tls_key, sizeof c->tls_key, "%.*s/key.pem", (int)dl, c->data_dir);
    snprintf(c->factor_key_file, sizeof c->factor_key_file, "%.*s/factor.key", (int)dl, c->data_dir);

    /* The storage policy: the design's numbers (storage.h), in the pass's
     * units. */
    {
        const uint64_t MB = 1024ull * 1024;
        oc_storage_policy *p = &c->storage;
        p->interval_ms      = 5 * 60 * 1000;
        p->max_age_ms       = 0;                        /* attachments are kept */
        p->grace_ms         = 24ull * 60 * 60 * 1000;   /* nothing younger than a day is evicted */
        p->reserve_bytes    = 256 * MB;                 /* the database's (REQ-216) */
        p->pressure_bytes   = 512 * MB;                 /* eviction starts below this (REQ-215) */
        p->batch            = 64;
        p->evict_enabled    = 1;
        p->audit_max_age_ms = 365ull * 24 * 60 * 60 * 1000;   /* REQ-251 */
    }
    c->sched_tick_ms = 15000;
    if (c->mode != OC_MODE_DEVELOPMENT) {
        /* Not development: the constants, whatever the development rows'
         * defaults wrote (the rows themselves were refused if set). */
        c->file_page        = (int)OC_MAX_FILE_LIST;
        c->call_max         = OC_CALL_MAX;
        c->max_conns_per_ip = OC_MAX_CONNS_PER_IP;
        memset(&c->dev, 0, sizeof c->dev);
    }

    /* Roots trusted beside the built-in ones: read and checked now, so a file
     * that does not parse stops the boot instead of failing each connection. */
    if (oc_tls_set_extra_ca(c->extra_ca) != 0) {
        snprintf(err, errcap, "OPENCHIME_EXTRA_CA=%s: must be a readable file of PEM certificates", c->extra_ca);
        return -1;
    }

    /* The relay is the control plane this box is enrolled with (AUTH.md §3):
     * its issuer is that origin and its keys are the ones it publishes. */
    if (c->auth_relay && !c->enroll.url) {
        snprintf(err, errcap, "OPENCHIME_AUTH=relay: the relay is the control plane; needs OPENCHIME_ENROLL_URL");
        return -1;
    }
    if (!c->oidc.pubkey) c->oidc.pubkey = "";

    /* Both carry signed requests: https, or plain http to loopback for a test. */
    if (c->enroll.url && !oc_url_transport_ok(c->enroll.url)) {
        snprintf(err, errcap, "OPENCHIME_ENROLL_URL=%s: must be https (plain http is for loopback only)", c->enroll.url);
        return -1;
    }
    if (c->push.url && !oc_url_transport_ok(c->push.url)) {
        snprintf(err, errcap, "OPENCHIME_PUSH_URL=%s: must be https (plain http is for loopback only)", c->push.url);
        return -1;
    }

    /* The certificate's source: central on a managed box, else self, unless
     * told; what each source needs is checked here, so a box that cannot get
     * the certificate it was told to get does not start instead of serving
     * the wrong one. */
    if (!c->tls_src.given && c->deployment_mode == OC_DEPLOY_MANAGED) c->tls_src.source = OC_TLS_SRC_CENTRAL;
    if (c->tls_src.source == OC_TLS_SRC_CENTRAL && !c->enroll.url) {
        snprintf(err, errcap, "OPENCHIME_TLS=central: a certificate through central needs an enrollment (OPENCHIME_ENROLL_URL)");
        return -1;
    }
    if (c->tls_src.source == OC_TLS_SRC_FILE && (access(c->tls_cert, R_OK) != 0 || access(c->tls_key, R_OK) != 0)) {
        snprintf(err, errcap, "OPENCHIME_TLS=file: needs %s and %s to be readable", c->tls_cert, c->tls_key);
        return -1;
    }

    /* Development is for a box of one's own: it may call itself managed (the
     * fixtures do, to exercise that mode), but it never enrols anywhere real. */
    if (c->mode == OC_MODE_DEVELOPMENT && c->enroll.url && strncmp(c->enroll.url, "http://", 7) != 0) {
        snprintf(err, errcap, "OPENCHIME_ENROLL_URL=%s: development mode enrols with a loopback test service only", c->enroll.url);
        return -1;
    }
    return 0;
}

int oc_config_load(char *err, size_t errcap) {
    char scratch[256];
    if (!err || !errcap) { err = scratch; errcap = sizeof scratch; }
    oc_config *c = &g_cfg;
    memset(c, 0, sizeof *c);
    memset(g_src, 0, sizeof g_src);

    if (scan_unknown(err, errcap) != 0) return -1;

    int slot = 0;
    for (const cfg_row *r = OC_CFG; r->name; r++) {
        int instances = (r->flags & CFG_PATTERN) ? r->count : 1;
        for (int i = 1; i <= instances; i++, slot++) {
            char name[64];
            if (r->flags & CFG_PATTERN) snprintf(name, sizeof name, r->name, i);
            else snprintf(name, sizeof name, "%s", r->name);
            const char *v = getenv(name);
            int is_default = 0;
            if (v && !*v) v = NULL;                 /* empty: unset */
            if (v) {
                /* OPENCHIME_MODE is the first row, so c->mode is known here. */
                if (r->cls == CFG_DEV && c->mode != OC_MODE_DEVELOPMENT) {
                    snprintf(err, errcap, "%s is read in development mode only, and OPENCHIME_MODE is %s", name, oc_mode_name(c->mode));
                    return -1;
                }
                /* The control plane's rows come with its ticket; a self-hoster
                 * setting one by hand is told so. Development, which stands
                 * in for a managed box in the fixtures, takes them as given. */
                if (r->cls == CFG_MANAGED && strcmp(name, "OPENCHIME_MANAGED_TICKET") != 0 && c->mode != OC_MODE_DEVELOPMENT) {
                    const char *t = getenv("OPENCHIME_MANAGED_TICKET");
                    if (!t || !*t) {
                        snprintf(err, errcap, "%s is the control plane's, set at provision time with OPENCHIME_MANAGED_TICKET (MANAGED.md)", name);
                        return -1;
                    }
                }
                if (!strcmp(name, "OPENCHIME_TLS")) c->tls_src.given = 1;
                if (slot < MAX_SLOTS) g_src[slot] = file_set(name) ? SRC_FILE : SRC_ENV;
            } else {
                v = r->dflt; is_default = 1;
            }
            if (store(c, r, name, v, is_default, err, errcap) != 0) return -1;
        }
    }
    return validate(c, err, errcap);
}

/* --- an EnvironmentFile ----------------------------------------------------------- */

int oc_config_apply_env_file(const char *path, char *err, size_t errcap) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(err, errcap, "%s: %s", path, strerror(errno)); return -1; }
    char line[4096];
    int ln = 0;
    g_n_file_names = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) p[--n] = '\0';
        if (!*p || *p == '#') continue;
        if (!strncmp(p, "export ", 7)) p += 7;
        char *eq = strchr(p, '=');
        if (!eq) { fclose(f); snprintf(err, errcap, "%s:%d: not NAME=value", path, ln); return -1; }
        *eq = '\0';
        char *name = p, *val = eq + 1;
        for (char *q = name; *q; q++)
            if (!isalnum((unsigned char)*q) && *q != '_') { fclose(f); snprintf(err, errcap, "%s:%d: '%s' is not a variable name", path, ln, name); return -1; }
        size_t vl = strlen(val);
        if (vl >= 2 && ((val[0] == '"' && val[vl - 1] == '"') || (val[0] == '\'' && val[vl - 1] == '\''))) {
            val[vl - 1] = '\0'; val++;
        }
        if (setenv(name, val, 1) != 0) { fclose(f); snprintf(err, errcap, "%s:%d: cannot set %s", path, ln, name); return -1; }
        if (g_n_file_names < (int)(sizeof g_file_names / sizeof g_file_names[0]) && strlen(name) < sizeof g_file_names[0])
            snprintf(g_file_names[g_n_file_names++], sizeof g_file_names[0], "%.63s", name);
    }
    fclose(f);
    return 0;
}

/* --- the views --------------------------------------------------------------------- */

void oc_config_report(void *out_) {
    FILE *out = out_;
    const oc_config *c = &g_cfg;
    int slot = 0;
    for (const cfg_row *r = OC_CFG; r->name; r++) {
        int instances = (r->flags & CFG_PATTERN) ? r->count : 1;
        for (int i = 1; i <= instances; i++, slot++) {
            char name[64], val[512] = "";
            if (r->flags & CFG_PATTERN) snprintf(name, sizeof name, r->name, i);
            else snprintf(name, sizeof name, "%s", r->name);
            const char *env = getenv(name);
            int src = slot < MAX_SLOTS ? g_src[slot] : SRC_DEFAULT;
            if ((r->flags & CFG_CLIENT) || r->kind == CFG_CUSTOM) {
                if (env && *env) snprintf(val, sizeof val, "%s", (r->flags & CFG_SECRET) ? "(set)" : env);
                else if (r->flags & CFG_CLIENT) continue;
                else snprintf(val, sizeof val, "%s", r->dflt ? r->dflt : "(unset)");
            } else {
                const void *p = (const char *)c + r->off;
                switch (r->kind) {
                case CFG_STR: case CFG_PATH: {
                    const char *s = *(const char *const *)p;
                    if (r->flags & CFG_SECRET) snprintf(val, sizeof val, "%s", s && *s ? "(set)" : "(unset)");
                    else snprintf(val, sizeof val, "%s", s ? (*s ? s : "\"\"") : "(unset)");
                    break; }
                case CFG_INT: snprintf(val, sizeof val, "%d", *(const int *)p); break;
                case CFG_U64: snprintf(val, sizeof val, "%llu", (unsigned long long)*(const uint64_t *)p); break;
                case CFG_BOOL: snprintf(val, sizeof val, "%s", *(const int *)p ? "on" : "off"); break;
                case CFG_ENUM: {
                    int idx = *(const int *)p, k = 0;
                    const char *e = r->enums;
                    while (e && k < idx) { e = strchr(e, '|'); if (e) e++; k++; }
                    if (e) { const char *bar = strchr(e, '|'); snprintf(val, sizeof val, "%.*s", bar ? (int)(bar - e) : (int)strlen(e), e); }
                    break; }
                default: break;
                }
            }
            fprintf(out, "%-38s = %-40s (%s%s)\n", name, val,
                    src == SRC_FILE ? "file" : src == SRC_ENV ? "environment" : "default",
                    r->cls == CFG_DEV ? ", development" : r->cls == CFG_MANAGED ? ", managed" : "");
        }
    }
}

static void print_group(FILE *out, const char *group, cfg_class cls) {
    int header = 0;
    for (const cfg_row *r = OC_CFG; r->name; r++) {
        if (r->cls != cls || strcmp(r->group, group) != 0) continue;
        if (!header) {
            fprintf(out, "\n### %s\n\n| Variable | Default | Meaning |\n|---|---|---|\n", group);
            header = 1;
        }
        char name[80];
        if (r->flags & CFG_PATTERN) {
            const char *pct = strstr(r->name, "%d");
            snprintf(name, sizeof name, "%.*s<n>%s", (int)(pct - r->name), r->name, pct + 2);
        } else snprintf(name, sizeof name, "%s", r->name);
        char dflt[80];
        if (!r->dflt) snprintf(dflt, sizeof dflt, "*(unset)*");
        else if (!*r->dflt) snprintf(dflt, sizeof dflt, "*(empty)*");
        else snprintf(dflt, sizeof dflt, "`%s`", r->dflt);
        char range[64] = "";
        if (r->kind == CFG_INT || r->kind == CFG_U64)
            snprintf(range, sizeof range, " Integer %lld..%lld.", (long long)r->min, (long long)r->max);
        else if (r->kind == CFG_BOOL) snprintf(range, sizeof range, " `on` or `off`.");
        else if (r->kind == CFG_ENUM) {
            snprintf(range, sizeof range, " One of `%s`.", r->enums);
            for (char *p = range; *p; p++) if (*p == '|') { memmove(p + 4, p + 1, strlen(p)); memcpy(p, "`, `", 4); p += 3; }
        }
        fprintf(out, "| `%s` | %s | %s%s |\n", name, dflt, r->doc, range);
    }
}

void oc_config_print_reference(void *out_) {
    FILE *out = out_;
    fprintf(out, "<!-- Generated by `openchimed --config-reference` from daemon/config_table.c; do not edit by hand (scripts/check/config.sh). -->\n");
    fprintf(out, "\n## Settings and features\n\nEverything an operator may set. A value that does not parse, is out of its range, or names a setting that does not exist stops the daemon before it opens anything; an empty value is unset. `openchimed --check-config` tests a file before a restart.\n");
    /* Operator rows, by group in table order. */
    const char *seen[32]; int n_seen = 0;
    for (const cfg_row *r = OC_CFG; r->name; r++) {
        int dup = 0;
        for (int i = 0; i < n_seen; i++) if (!strcmp(seen[i], r->group)) dup = 1;
        if (dup || n_seen >= 32) continue;
        seen[n_seen++] = r->group;
    }
    for (int i = 0; i < n_seen; i++) print_group(out, seen[i], CFG_OPERATOR);
    fprintf(out, "\n## Managed\n\nWritten by the control plane when it provisions a managed box (MANAGED.md), with `OPENCHIME_MANAGED_TICKET`; set without the ticket, each refuses the start.\n");
    for (int i = 0; i < n_seen; i++) print_group(out, seen[i], CFG_MANAGED);
    fprintf(out, "\n## Development mode only\n\nRead by the daemon or a client only when `OPENCHIME_MODE=development` (TESTING.md): the tests, the harnesses and the scripts. In any other mode each refuses the daemon's start, and a client ignores it.\n");
    for (int i = 0; i < n_seen; i++) print_group(out, seen[i], CFG_DEV);
    fprintf(out, "\n## No longer read\n\nA name that was a setting once. Set, it stops the daemon with the line below; the constants it names are in `daemon/config.h`.\n\n| Name | Instead |\n|---|---|\n");
    for (const cfg_removed *x = OC_CFG_REMOVED; x->name; x++) fprintf(out, "| `%s` | %s |\n", x->name, x->instead);
}

void oc_config_print_env_template(void *out_) {
    FILE *out = out_;
    fprintf(out,
        "# OpenChime daemon configuration.\n"
        "#\n"
        "# The daemon is configured entirely from its environment (ARCH-26); this is\n"
        "# the EnvironmentFile systemd hands it. Every setting an operator may make\n"
        "# is here, at its default, commented; uncomment to change one. A value that\n"
        "# does not parse or is out of range stops the daemon: test an edit with\n"
        "#   openchimed --check-config --env-file /etc/openchime/openchimed.env\n"
        "# docs/CONFIG.md explains each. Generated by `openchimed --config-env`.\n"
        "#\n"
        "# This file is a package conffile: your edits survive an upgrade.\n");
    const char *seen[32]; int n_seen = 0;
    for (const cfg_row *r = OC_CFG; r->name; r++) {
        int dup = 0;
        for (int i = 0; i < n_seen; i++) if (!strcmp(seen[i], r->group)) dup = 1;
        if (dup || n_seen >= 32 || r->cls != CFG_OPERATOR || (r->flags & CFG_CLIENT)) continue;
        seen[n_seen++] = r->group;
    }
    for (int gi = 0; gi < n_seen; gi++) {
        fprintf(out, "\n# --- %s ---\n", seen[gi]);
        for (const cfg_row *r = OC_CFG; r->name; r++) {
            if (r->cls != CFG_OPERATOR || (r->flags & CFG_CLIENT) || strcmp(r->group, seen[gi]) != 0) continue;
            fprintf(out, "# %s\n", r->doc);
            /* The paths the packages repoint into the unit's StateDirectory, set. */
            const char *packaged = NULL;
            if (!strcmp(r->name, "OPENCHIME_DATA_DIR")) packaged = "/var/lib/openchime";
            if (r->flags & CFG_PATTERN) {
                char name[80]; const char *pct = strstr(r->name, "%d");
                snprintf(name, sizeof name, "%.*s1", (int)(pct - r->name), r->name);
                fprintf(out, "#%s=\n", name);
            } else if (packaged) fprintf(out, "%s=%s\n", r->name, packaged);
            else fprintf(out, "#%s=%s\n", r->name, r->dflt ? r->dflt : "");
        }
    }
}
