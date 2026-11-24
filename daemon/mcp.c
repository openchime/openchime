/* The MCP surface's pure parts: see mcp.h and docs/details/MCP.md. */

#include "mcp.h"
#include "json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- JSON out --------------------------------------------------------------- */

void oc_mcp_buf_free(oc_mcp_buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

void oc_mcp_putn(oc_mcp_buf *b, const char *s, size_t n) {
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 1024;
        while (nc < b->len + n + 1) nc *= 2;
        char *g = realloc(b->p, nc);
        if (!g) { b->failed = 1; return; }
        b->p = g; b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void oc_mcp_put(oc_mcp_buf *b, const char *s) { oc_mcp_putn(b, s, strlen(s)); }

void oc_mcp_putf(oc_mcp_buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) { b->failed = 1; return; }
    if ((size_t)n < sizeof tmp) { oc_mcp_putn(b, tmp, (size_t)n); return; }
    char *big = malloc((size_t)n + 1);
    if (!big) { b->failed = 1; return; }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    oc_mcp_putn(b, big, (size_t)n);
    free(big);
}

void oc_mcp_put_str(oc_mcp_buf *b, const char *s, size_t n) {
    oc_mcp_putn(b, "\"", 1);
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        const char *esc = NULL;
        char u[8];
        if (ch == '"') esc = "\\\"";
        else if (ch == '\\') esc = "\\\\";
        else if (ch == '\n') esc = "\\n";
        else if (ch == '\r') esc = "\\r";
        else if (ch == '\t') esc = "\\t";
        else if (ch < 0x20) { snprintf(u, sizeof u, "\\u%04x", ch); esc = u; }
        if (!esc) continue;
        oc_mcp_putn(b, s + run, i - run);
        oc_mcp_put(b, esc);
        run = i + 1;
    }
    oc_mcp_putn(b, s + run, n - run);
    oc_mcp_putn(b, "\"", 1);
}

void oc_mcp_put_time(oc_mcp_buf *b, uint64_t ms) {
    time_t t = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&t, &tm);
    oc_mcp_putf(b, "\"%04d-%02d-%02dT%02d:%02d:%02d.%03uZ\"", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, (unsigned)(ms % 1000));
}

/* --- time in -------------------------------------------------------------- */

static int digits(const char *s, int n, int *out) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

/* Days from 1970-01-01 to y-m-d, proleptic Gregorian (Howard Hinnant's). */
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int oc_mcp_parse_time(const char *s, uint64_t *ms) {
    size_t n = strlen(s);
    int Y, M, D, h, mi, se;
    if (n < 20 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != 't') || s[13] != ':' || s[16] != ':')
        return -1;
    if (digits(s, 4, &Y) || digits(s + 5, 2, &M) || digits(s + 8, 2, &D) || digits(s + 11, 2, &h) ||
        digits(s + 14, 2, &mi) || digits(s + 17, 2, &se))
        return -1;
    if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || mi > 59 || se > 60) return -1;
    size_t i = 19;
    int frac = 0, fd = 0;
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') { if (fd < 3) { frac = frac * 10 + (s[i] - '0'); fd++; } i++; }
        while (fd < 3) { frac *= 10; fd++; }
    }
    int64_t off = 0;
    if (i < n && (s[i] == 'Z' || s[i] == 'z')) i++;
    else if (i + 6 == n && (s[i] == '+' || s[i] == '-') && s[i + 3] == ':') {
        int oh, om;
        if (digits(s + i + 1, 2, &oh) || digits(s + i + 4, 2, &om)) return -1;
        off = (int64_t)(oh * 60 + om) * 60 * (s[i] == '+' ? 1 : -1);
        i += 6;
    } else return -1;
    if (i != n) return -1;
    int64_t secs = days_from_civil(Y, M, D) * 86400 + h * 3600 + mi * 60 + se - off;
    if (secs < 0) return -1;
    *ms = (uint64_t)secs * 1000 + (uint64_t)frac;
    return 0;
}

/* --- requests ------------------------------------------------------------- */

void oc_mcp_call_free(oc_mcp_call *c) { free(c->text); c->text = NULL; c->text_len = 0; }

void oc_mcp_error(oc_mcp_buf *b, const char *id, int code, const char *message, const char *data_json) {
    oc_mcp_put(b, "{\"jsonrpc\":\"2.0\",\"id\":");
    oc_mcp_put(b, id && id[0] ? id : "null");
    oc_mcp_putf(b, ",\"error\":{\"code\":%d,\"message\":", code);
    oc_mcp_put_str(b, message, strlen(message));
    if (data_json) { oc_mcp_put(b, ",\"data\":"); oc_mcp_put(b, data_json); }
    oc_mcp_put(b, "}}");
}

static int span_eq(const char *a, size_t an, const char *b) { return an == strlen(b) && memcmp(a, b, an) == 0; }

static int unsupported(oc_mcp_buf *err, const char *id) {
    oc_mcp_error(err, id, -32602, "UnsupportedProtocolVersionError",
                 "{\"supported\":[\"" OC_MCP_REVISION "\"]}");
    return 400;
}

static int arg_u64(const oc_json *d, int args, const char *key, uint64_t *out, int *has) {
    int t = oc_json_get(d, args, key);
    if (t < 0) return 0;
    if (oc_json_u64(d, t, out) == 0) { *has = 1; return 0; }
    /* An id as a string is taken too: some clients quote every id. */
    char s[32];
    if (oc_json_str(d, t, s, sizeof s) != 0 || !s[0]) return -1;
    uint64_t v = 0;
    for (char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (uint64_t)(*p - '0');
    }
    *out = v;
    *has = 1;
    return 0;
}

static int arg_str(const oc_json *d, int args, const char *key, char *out, size_t cap) {
    int t = oc_json_get(d, args, key);
    if (t < 0) { out[0] = '\0'; return 0; }
    if (d->t[t].type == JSMN_PRIMITIVE) {               /* a channel given as its id */
        size_t n = (size_t)(d->t[t].end - d->t[t].start);
        if (n == 0 || n >= cap) return -1;
        memcpy(out, d->js + d->t[t].start, n);
        out[n] = '\0';
        for (size_t i = 0; i < n; i++) if (out[i] < '0' || out[i] > '9') return -1;
        return 0;
    }
    return oc_json_str(d, t, out, cap);
}

static int arg_time(const oc_json *d, int args, const char *key, uint64_t *out, int *has) {
    char s[64];
    if (arg_str(d, args, key, s, sizeof s) != 0) return -1;
    if (!s[0]) return 0;
    if (oc_mcp_parse_time(s, out) != 0) return -1;
    *has = 1;
    return 0;
}

static void read_args(const oc_json *d, int args, oc_mcp_call *c) {
#define BAD(msg) do { snprintf(c->arg_error, sizeof c->arg_error, "%s", (msg)); return; } while (0)
    if (args < 0 || d->t[args].type != JSMN_OBJECT) {
        if (args >= 0) BAD("arguments must be an object");
        args = -1;
    }
    if (args < 0) {
        if (c->tool != OC_MCP_SEARCH) BAD("channel is required");
        BAD("query is required");
    }
    if (arg_str(d, args, "channel", c->channel, sizeof c->channel) != 0) BAD("channel is not a name or an id");
    uint64_t lim = 0;
    int has_lim = 0;
    if (arg_u64(d, args, "limit", &lim, &has_lim) != 0) BAD("limit must be a number");
    if (c->tool == OC_MCP_READ_HISTORY) {
        if (!c->channel[0]) BAD("channel is required");
        int h = 0;
        if (arg_u64(d, args, "thread", &c->thread, &h) != 0) BAD("thread must be a message id");
        if (arg_u64(d, args, "before", &c->before, &c->has_before) != 0) BAD("before must be a cursor or a message id");
        if (arg_u64(d, args, "after", &c->after, &c->has_after) != 0) BAD("after must be a cursor or a message id");
        if (c->has_before && c->has_after) BAD("give at most one of before and after");
        if (arg_time(d, args, "since", &c->since_ms, &c->has_since) != 0) BAD("since must be an RFC 3339 time");
        if (has_lim && (lim < 1 || lim > OC_MCP_HISTORY_MAX)) BAD("limit must be 1 to 200");
        c->limit = has_lim ? (int)lim : OC_MCP_HISTORY_DEFAULT;
    } else if (c->tool == OC_MCP_SEARCH) {
        if (arg_str(d, args, "query", c->query, sizeof c->query) != 0 || !c->query[0]) BAD("query is required");
        if (arg_str(d, args, "from", c->from, sizeof c->from) != 0) BAD("from must be a handle");
        if (arg_time(d, args, "since", &c->since_ms, &c->has_since) != 0) BAD("since must be an RFC 3339 time");
        if (arg_time(d, args, "until", &c->until_ms, &c->has_until) != 0) BAD("until must be an RFC 3339 time");
        if (arg_str(d, args, "cursor", c->cursor, sizeof c->cursor) != 0) BAD("cursor is not one this gave");
        if (has_lim && (lim < 1 || lim > OC_MCP_SEARCH_MAX)) BAD("limit must be 1 to 50");
        c->limit = has_lim ? (int)lim : OC_MCP_SEARCH_DEFAULT;
    } else {
        if (!c->channel[0]) BAD("channel is required");
        int h = 0;
        if (arg_u64(d, args, "thread", &c->thread, &h) != 0) BAD("thread must be a message id");
        int t = oc_json_get(d, args, "text");
        if (t < 0 || d->t[t].type != JSMN_STRING) BAD("text is required");
        size_t raw = (size_t)(d->t[t].end - d->t[t].start);
        c->text = malloc(raw + 1);
        if (!c->text) BAD("out of memory");
        long tl = oc_json_unescape(d->js + d->t[t].start, raw, c->text, raw + 1);
        if (tl <= 0) BAD("text must not be empty");
        c->text_len = (size_t)tl;
        c->text[c->text_len] = '\0';
        if (arg_str(d, args, "key", c->key, sizeof c->key) != 0 || strlen(c->key) > 64)
            BAD("key must be at most 64 characters");
    }
#undef BAD
}

int oc_mcp_parse(const char *body, size_t len, const oc_mcp_headers *h, oc_mcp_call *out, oc_mcp_buf *err) {
    memset(out, 0, sizeof *out);
    /* A page in someone's browser must not drive the endpoint: an Origin other
     * than the workspace's own https origin is refused (an agent sends none). */
    if (h->origin && h->origin_len) {
        int same = h->host && h->origin_len == 8 + h->host_len && memcmp(h->origin, "https://", 8) == 0 &&
                   memcmp(h->origin + 8, h->host, h->host_len) == 0;
        if (!same) { oc_mcp_error(err, NULL, -32600, "Origin not allowed", NULL); return 403; }
    }
    oc_json d;
    if (!body || !len || oc_json_parse(&d, body, len) != 0) {
        oc_mcp_error(err, NULL, -32700, "Parse error", NULL);
        return 400;
    }
    int rc = 0;
    if (d.n < 1 || d.t[0].type != JSMN_OBJECT) { oc_mcp_error(err, NULL, -32600, "Invalid Request", NULL); rc = 400; goto done; }
    int it = oc_json_get(&d, 0, "id");
    if (it >= 0) {
        size_t n = (size_t)(d.t[it].end - d.t[it].start);
        int quoted = d.t[it].type == JSMN_STRING;
        if (n + 2 >= sizeof out->id || (d.t[it].type != JSMN_STRING && d.t[it].type != JSMN_PRIMITIVE)) {
            oc_mcp_error(err, NULL, -32600, "Invalid Request", NULL); rc = 400; goto done;
        }
        snprintf(out->id, sizeof out->id, quoted ? "\"%.*s\"" : "%.*s", (int)n, d.js + d.t[it].start);
    }
    char method[64];
    if (oc_json_get_str(&d, 0, "method", method, sizeof method) != 0) {
        oc_mcp_error(err, out->id, -32600, "Invalid Request", NULL); rc = 400; goto done;
    }
    /* The earlier era's handshake: refused with the revision this speaks. */
    if (!strcmp(method, "initialize")) { rc = unsupported(err, out->id); goto done; }
    int params = oc_json_get(&d, 0, "params");
    if (params >= 0 && d.t[params].type != JSMN_OBJECT) {
        oc_mcp_error(err, out->id, -32602, "params must be an object", NULL); rc = 400; goto done;
    }
    char ver[32] = "";
    int meta = params >= 0 ? oc_json_get(&d, params, "_meta") : -1;
    if (meta >= 0 && d.t[meta].type == JSMN_OBJECT)
        (void)oc_json_get_str(&d, meta, "io.modelcontextprotocol/protocolVersion", ver, sizeof ver);
    if (!h->version || !h->version_len || !ver[0] || strcmp(ver, OC_MCP_REVISION) != 0 ||
        !span_eq(h->version, h->version_len, OC_MCP_REVISION)) {
        /* Both must name it; a header that names another than the body is a
         * mismatch, and either naming another revision is unsupported. */
        if (ver[0] && h->version && h->version_len && !strcmp(ver, OC_MCP_REVISION) &&
            !span_eq(h->version, h->version_len, ver)) {
            oc_mcp_error(err, out->id, OC_MCP_HEADER_MISMATCH, "HeaderMismatch", NULL); rc = 400; goto done;
        }
        rc = unsupported(err, out->id);
        goto done;
    }
    if (!h->method || !span_eq(h->method, h->method_len, method)) {
        oc_mcp_error(err, out->id, OC_MCP_HEADER_MISMATCH, "HeaderMismatch", NULL); rc = 400; goto done;
    }
    if (!strcmp(method, "server/discover")) out->method = OC_MCP_DISCOVER;
    else if (!strcmp(method, "tools/list")) out->method = OC_MCP_TOOLS_LIST;
    else if (!strcmp(method, "tools/call")) out->method = OC_MCP_TOOLS_CALL;
    else { oc_mcp_error(err, out->id, -32601, "Method not found", NULL); rc = 404; goto done; }
    if (out->method != OC_MCP_TOOLS_CALL) goto done;

    char name[32];
    if (params < 0 || oc_json_get_str(&d, params, "name", name, sizeof name) != 0) {
        oc_mcp_error(err, out->id, -32602, "name is required", NULL); rc = 400; goto done;
    }
    if (!h->name || !span_eq(h->name, h->name_len, name)) {
        oc_mcp_error(err, out->id, OC_MCP_HEADER_MISMATCH, "HeaderMismatch", NULL); rc = 400; goto done;
    }
    if (!strcmp(name, "read_history")) out->tool = OC_MCP_READ_HISTORY;
    else if (!strcmp(name, "search")) out->tool = OC_MCP_SEARCH;
    else if (!strcmp(name, "post")) out->tool = OC_MCP_POST;
    else { oc_mcp_error(err, out->id, -32602, "Unknown tool", NULL); rc = 400; goto done; }
    read_args(&d, oc_json_get(&d, params, "arguments"), out);
done:
    oc_json_free(&d);
    return rc;
}

/* --- results -------------------------------------------------------------- */

static void open_result(oc_mcp_buf *b, const char *id) {
    oc_mcp_put(b, "{\"jsonrpc\":\"2.0\",\"id\":");
    oc_mcp_put(b, id && id[0] ? id : "null");
    oc_mcp_put(b, ",\"result\":{");
}

static void close_result(oc_mcp_buf *b, const char *server_version) {
    oc_mcp_put(b, ",\"resultType\":\"complete\",\"_meta\":{\"io.modelcontextprotocol/serverInfo\":{\"name\":\"openchimed\",\"version\":");
    oc_mcp_put_str(b, server_version ? server_version : "", server_version ? strlen(server_version) : 0);
    oc_mcp_put(b, "}}}}");
}

void oc_mcp_discover(oc_mcp_buf *b, const char *id, const char *server_version) {
    open_result(b, id);
    oc_mcp_put(b, "\"supportedVersions\":[\"" OC_MCP_REVISION "\"],\"capabilities\":{\"tools\":{}},"
                  "\"serverInfo\":{\"name\":\"openchimed\",\"version\":");
    oc_mcp_put_str(b, server_version ? server_version : "", server_version ? strlen(server_version) : 0);
    oc_mcp_put(b, "}");
    close_result(b, server_version);
}

static void channel_schema(oc_mcp_buf *b, const char *const *names, size_t n) {
    oc_mcp_put(b, "\"channel\":{\"description\":\"The conversation, by name or numeric id.\",\"anyOf\":[{\"type\":\"string\",\"enum\":[");
    for (size_t i = 0; i < n; i++) {
        if (i) oc_mcp_put(b, ",");
        oc_mcp_put_str(b, names[i], strlen(names[i]));
    }
    oc_mcp_put(b, "]},{\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\"}]}");
}

void oc_mcp_tools_list(oc_mcp_buf *b, const char *id, const char *server_version,
                       const char *const *names, size_t n) {
    open_result(b, id);
    oc_mcp_put(b, "\"tools\":[");
    /* read_history */
    oc_mcp_put(b, "{\"name\":\"read_history\",\"description\":\"Read a conversation's messages, or one thread's "
                  "replies, newest first by page.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{");
    channel_schema(b, names, n);
    oc_mcp_put(b, ",\"thread\":{\"type\":\"integer\",\"description\":\"A message id: read that thread's replies.\"}"
                  ",\"before\":{\"type\":\"integer\",\"description\":\"A cursor or message id: older messages.\"}"
                  ",\"after\":{\"type\":\"integer\",\"description\":\"A cursor or message id: newer messages.\"}"
                  ",\"since\":{\"type\":\"string\",\"format\":\"date-time\"}"
                  ",\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200,\"default\":50}"
                  "},\"required\":[\"channel\"]}},");
    /* search */
    oc_mcp_put(b, "{\"name\":\"search\",\"description\":\"Search the conversations you can read, best match "
                  "first.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                  "\"query\":{\"type\":\"string\"},");
    channel_schema(b, names, n);
    oc_mcp_put(b, ",\"from\":{\"type\":\"string\",\"description\":\"Only messages by this author.\"}"
                  ",\"since\":{\"type\":\"string\",\"format\":\"date-time\"}"
                  ",\"until\":{\"type\":\"string\",\"format\":\"date-time\"}"
                  ",\"cursor\":{\"type\":\"string\"}"
                  ",\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":50,\"default\":20}"
                  "},\"required\":[\"query\"]}},");
    /* post */
    oc_mcp_put(b, "{\"name\":\"post\",\"description\":\"Post a message, or a reply in a thread, as "
                  "yourself.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{");
    channel_schema(b, names, n);
    oc_mcp_put(b, ",\"text\":{\"type\":\"string\",\"maxLength\":65536}"
                  ",\"thread\":{\"type\":\"integer\"}"
                  ",\"key\":{\"type\":\"string\",\"maxLength\":64,\"description\":\"An idempotency key: the same key "
                  "posts once.\"}"
                  "},\"required\":[\"channel\",\"text\"]}}");
    oc_mcp_putf(b, "],\"cacheScope\":\"private\",\"ttlMs\":%u", OC_MCP_TTL_MS);
    close_result(b, server_version);
}

void oc_mcp_tool_result(oc_mcp_buf *b, const char *id, const char *server_version, const char *structured,
                        int is_error) {
    open_result(b, id);
    oc_mcp_put(b, "\"content\":[{\"type\":\"text\",\"text\":");
    oc_mcp_put_str(b, structured, strlen(structured));
    oc_mcp_put(b, "}],\"structuredContent\":");
    oc_mcp_put(b, structured);
    oc_mcp_put(b, is_error ? ",\"isError\":true" : ",\"isError\":false");
    close_result(b, server_version);
}

void oc_mcp_tool_error(oc_mcp_buf *b, const char *id, const char *server_version, const char *message) {
    oc_mcp_buf s = { 0 };
    oc_mcp_put(&s, "{\"error\":");
    oc_mcp_put_str(&s, message, strlen(message));
    oc_mcp_put(&s, "}");
    oc_mcp_tool_result(b, id, server_version, s.failed ? "{}" : s.p, 1);
    oc_mcp_buf_free(&s);
}

/* --- text ----------------------------------------------------------------- */

static int is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

size_t oc_mcp_cut(const char *text, size_t len, size_t limit, size_t *chars) {
    size_t count = 0, at_limit = len, last_nl = 0;
    int nl_seen = 0;
    for (size_t i = 0; i < len; i++) {
        if (is_cont((unsigned char)text[i])) continue;
        if (count == limit && at_limit == len) at_limit = i;
        if (count < limit && text[i] == '\n') { last_nl = i; nl_seen = 1; }
        count++;
    }
    if (chars) *chars = count;
    if (count <= limit) return len;
    return nl_seen && last_nl > 0 ? last_nl : at_limit;
}

static const char *find_ci(const char *h, size_t hn, const char *nd, size_t nn) {
    if (!nn || nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t k = 0;
        while (k < nn && tolower((unsigned char)h[i + k]) == tolower((unsigned char)nd[k])) k++;
        if (k == nn) return h + i;
    }
    return NULL;
}

size_t oc_mcp_excerpt(const char *text, size_t len, const char *query, char *out, size_t cap) {
    if (!cap) return 0;
    /* The first word of the query, without quotes or operators. */
    const char *q = query ? query : "";
    while (*q && (isspace((unsigned char)*q) || *q == '"')) q++;
    size_t qn = 0;
    while (q[qn] && !isspace((unsigned char)q[qn]) && q[qn] != '"') qn++;
    const char *hit = find_ci(text, len, q, qn);
    size_t start = 0;
    if (hit) {
        /* Begin about a third of the window before the match, on a character. */
        size_t at = (size_t)(hit - text), back = OC_MCP_EXCERPT_MAX / 3, seen = 0;
        start = at;
        while (start > 0 && seen < back) { start--; if (!is_cont((unsigned char)text[start])) seen++; }
        while (start < len && is_cont((unsigned char)text[start])) start++;
    }
    size_t end = start, chars = 0;
    while (end < len && chars < OC_MCP_EXCERPT_MAX) {
        end++;
        while (end < len && is_cont((unsigned char)text[end])) end++;
        chars++;
    }
    size_t n = end - start;
    if (n >= cap) n = cap - 1;
    while (n > 0 && start + n < len && is_cont((unsigned char)text[start + n])) n--;
    memcpy(out, text + start, n);
    out[n] = '\0';
    return n;
}

/* --- a page ------------------------------------------------------------------ */

static void put_msg(oc_mcp_buf *b, const oc_mcp_msg *m, int may_cut, int as_search, const char *query) {
    oc_mcp_putf(b, "{\"id\":%llu,\"author\":{\"name\":", (unsigned long long)m->id);
    oc_mcp_put_str(b, m->author ? m->author : "", m->author ? strlen(m->author) : 0);
    oc_mcp_put(b, ",\"handle\":");
    oc_mcp_put_str(b, m->handle ? m->handle : "", m->handle ? strlen(m->handle) : 0);
    oc_mcp_put(b, "},\"author_kind\":");
    oc_mcp_put_str(b, m->author_kind ? m->author_kind : "person", strlen(m->author_kind ? m->author_kind : "person"));
    oc_mcp_put(b, ",\"at\":");
    oc_mcp_put_time(b, m->at_ms);
    if (as_search) {
        if (m->channel) {
            oc_mcp_put(b, ",\"channel\":");
            oc_mcp_put_str(b, m->channel, strlen(m->channel));
        }
        oc_mcp_putf(b, ",\"channel_id\":%llu", (unsigned long long)m->channel_id);
        char ex[OC_MCP_EXCERPT_MAX * 4 + 4];
        size_t en = oc_mcp_excerpt(m->text ? m->text : "", m->text ? m->text_len : 0, query, ex, sizeof ex);
        oc_mcp_put(b, ",\"excerpt\":");
        oc_mcp_put_str(b, ex, en);
    } else {
        size_t chars = 0;
        size_t keep = may_cut ? oc_mcp_cut(m->text ? m->text : "", m->text_len, OC_MCP_CUT_AT, &chars) : m->text_len;
        oc_mcp_put(b, ",\"text\":");
        oc_mcp_put_str(b, m->text ? m->text : "", keep);
        if (keep < m->text_len) oc_mcp_putf(b, ",\"truncated\":{\"length\":%zu}", chars);
    }
    if (m->edited) oc_mcp_put(b, ",\"edited\":true");
    if (m->replies) oc_mcp_putf(b, ",\"replies\":%u", m->replies);
    if (m->n_files) {
        oc_mcp_put(b, ",\"files\":[");
        for (size_t i = 0; i < m->n_files; i++) {
            if (i) oc_mcp_put(b, ",");
            oc_mcp_put(b, "{\"name\":");
            oc_mcp_put_str(b, m->file_names[i], strlen(m->file_names[i]));
            oc_mcp_put(b, ",\"type\":");
            oc_mcp_put_str(b, m->file_types[i], strlen(m->file_types[i]));
            oc_mcp_putf(b, ",\"size\":%llu}", (unsigned long long)m->file_sizes[i]);
        }
        oc_mcp_put(b, "]");
    }
    oc_mcp_put(b, "}");
}

static size_t text_cost(const oc_mcp_msg *m, int as_search) {
    if (as_search) return OC_MCP_EXCERPT_MAX * 4;
    size_t chars;
    return oc_mcp_cut(m->text ? m->text : "", m->text_len, OC_MCP_CUT_AT, &chars);
}

size_t oc_mcp_page(oc_mcp_buf *b, const oc_mcp_msg *msgs, size_t n, int newest_first_kept,
                   int more_older, int more_newer, int as_search, const char *query) {
    /* Which run fits the text budget: from the newest end backwards, or from
     * the oldest end forwards; one message always. */
    size_t lo = 0, hi = n, budget = 0;          /* kept: [lo, hi) */
    if (n) {
        if (newest_first_kept) {
            lo = n;
            while (lo > 0) {
                size_t c = text_cost(&msgs[lo - 1], as_search);
                if (lo < n && budget + c > OC_MCP_PAGE_MAX) break;
                budget += c;
                lo--;
            }
        } else {
            hi = 0;
            while (hi < n) {
                size_t c = text_cost(&msgs[hi], as_search);
                if (hi > 0 && budget + c > OC_MCP_PAGE_MAX) break;
                budget += c;
                hi++;
            }
        }
    }
    size_t kept = hi - lo;
    oc_mcp_put(b, "\"messages\":[");
    for (size_t i = lo; i < hi; i++) {
        if (i > lo) oc_mcp_put(b, ",");
        put_msg(b, &msgs[i], kept > 1, as_search, query);
    }
    oc_mcp_put(b, "]");
    if (!as_search && kept) {
        if (more_older || lo > 0) oc_mcp_putf(b, ",\"older\":%llu", (unsigned long long)msgs[lo].id);
        if (more_newer || hi < n) oc_mcp_putf(b, ",\"newer\":%llu", (unsigned long long)msgs[hi - 1].id);
    }
    return kept;
}

/* --- the budget ----------------------------------------------------------- */

unsigned oc_mcp_bucket_take(oc_mcp_bucket *b, uint64_t now_ms, unsigned cost, unsigned per_minute, unsigned burst) {
    if (!per_minute) per_minute = 1;
    if (!b->primed) { b->level = burst; b->at_ms = now_ms; b->primed = 1; }
    if (now_ms > b->at_ms) {
        b->level += (double)(now_ms - b->at_ms) * per_minute / 60000.0;
        if (b->level > burst) b->level = burst;
        b->at_ms = now_ms;
    }
    if (b->level + 1e-9 >= cost) { b->level -= cost; return 0; }
    double need = cost - b->level;
    unsigned secs = (unsigned)(need * 60.0 / per_minute + 0.999999);
    return secs ? secs : 1;
}
