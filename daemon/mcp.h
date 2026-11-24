/*
 * The MCP surface's pure parts (docs/details/MCP.md, ARCH-120): a request read
 * and checked, the tools described, results written as JSON, a page of
 * messages bounded and cut, a search excerpt, and the per-token budget. No
 * I/O, no database, no globals: the event loop (daemon/netloop.c) authenticates,
 * reads through the writer's readers and answers with what is built here.
 *
 * One revision, 2026-07-28: stateless, one JSON-RPC request per POST and one
 * JSON object back, tools only.
 */
#ifndef OC_MCP_H
#define OC_MCP_H

#include <stddef.h>
#include <stdint.h>

#define OC_MCP_REVISION        "2026-07-28"
#define OC_MCP_TTL_MS          60000u        /* tools/list may be cached this long */
#define OC_MCP_HISTORY_DEFAULT 50
#define OC_MCP_HISTORY_MAX     200
#define OC_MCP_SEARCH_DEFAULT  20
#define OC_MCP_SEARCH_MAX      50
#define OC_MCP_EXCERPT_MAX     300u          /* characters */
#define OC_MCP_PAGE_MAX        65536u        /* bytes of text in one page */
#define OC_MCP_CUT_AT          8000u         /* characters of one message's text */
#define OC_MCP_SEARCH_COST     3u            /* a search costs three reads */
#define OC_MCP_HEADER_MISMATCH (-32020)

typedef enum { OC_MCP_DISCOVER = 1, OC_MCP_TOOLS_LIST, OC_MCP_TOOLS_CALL } oc_mcp_method;
typedef enum { OC_MCP_TOOL_NONE = 0, OC_MCP_READ_HISTORY, OC_MCP_SEARCH, OC_MCP_POST } oc_mcp_tool;

/* What the request's headers said, as the I/O thread passed them. */
typedef struct {
    const char *version; size_t version_len;   /* MCP-Protocol-Version */
    const char *method;  size_t method_len;    /* Mcp-Method */
    const char *name;    size_t name_len;      /* Mcp-Name */
    const char *origin;  size_t origin_len;
    const char *host;    size_t host_len;
} oc_mcp_headers;

/* One request, read. Strings are NUL-terminated copies; `text` is heap (free
 * with oc_mcp_call_free). An argument absent is 0 / "" with its has_ flag 0. */
typedef struct {
    oc_mcp_method method;
    oc_mcp_tool   tool;
    char          id[80];        /* the JSON-RPC id, as JSON text ("1", "\"a\"") */
    char          channel[160];
    uint64_t      thread, before, after;
    int           has_before, has_after;
    uint64_t      since_ms, until_ms;
    int           has_since, has_until;
    int           limit;         /* 0: the tool's default */
    char          query[512];
    char          from[160];
    char          cursor[64];
    char         *text;
    size_t        text_len;
    char          key[72];
    /* A tools/call whose arguments are wrong: answered as a tool error with
     * this sentence, "" when they are right. */
    char          arg_error[160];
} oc_mcp_call;

void oc_mcp_call_free(oc_mcp_call *c);

/* A growing JSON text. */
typedef struct { char *p; size_t len, cap; int failed; } oc_mcp_buf;
void oc_mcp_buf_free(oc_mcp_buf *b);
void oc_mcp_put(oc_mcp_buf *b, const char *s);
void oc_mcp_putn(oc_mcp_buf *b, const char *s, size_t n);
void oc_mcp_putf(oc_mcp_buf *b, const char *fmt, ...);
/* `n` bytes of UTF-8 as a JSON string, quotes included. */
void oc_mcp_put_str(oc_mcp_buf *b, const char *s, size_t n);
/* A time in ms since the epoch as RFC 3339 in UTC, quoted. */
void oc_mcp_put_time(oc_mcp_buf *b, uint64_t ms);

/* Read and check a request: the revision in the header and in `_meta`, the
 * headers against the body, the Origin, and the method and its arguments.
 * Returns 0 with `out` filled, or the HTTP status to answer with `err` (a
 * whole JSON body) filled. `out` must be freed either way. */
int oc_mcp_parse(const char *body, size_t len, const oc_mcp_headers *h, oc_mcp_call *out, oc_mcp_buf *err);

/* RFC 3339 to ms since the epoch: 0, or -1. */
int oc_mcp_parse_time(const char *s, uint64_t *ms);

/* A JSON-RPC error body for `id` (JSON text, or NULL for null). */
void oc_mcp_error(oc_mcp_buf *b, const char *id, int code, const char *message, const char *data_json);

/* The result envelopes. Each writes a whole JSON-RPC response. */
void oc_mcp_discover(oc_mcp_buf *b, const char *id, const char *server_version);
/* `names`: the conversations the caller can read, by name ("#general"). */
void oc_mcp_tools_list(oc_mcp_buf *b, const char *id, const char *server_version,
                       const char *const *names, size_t n_names);
/* A tool's result: `structured` is a JSON object's text; `is_error` makes it
 * a tool error with `structured` as {"error": ...}. */
void oc_mcp_tool_result(oc_mcp_buf *b, const char *id, const char *server_version, const char *structured,
                        int is_error);
/* A tool error with a plain message. */
void oc_mcp_tool_error(oc_mcp_buf *b, const char *id, const char *server_version, const char *message);

/* One message as a tool returns it. */
typedef struct {
    uint64_t    id;
    const char *author;        /* display name */
    const char *handle;        /* "" when it has none */
    const char *author_kind;   /* "person", "agent" or "webhook" */
    uint64_t    at_ms;
    const char *text;
    size_t      text_len;
    int         edited;
    uint32_t    replies;
    const char *channel;       /* search only: the conversation's name, else NULL */
    uint64_t    channel_id;
    /* Files: names, types and sizes, not content. */
    size_t      n_files;
    const char *const *file_names;
    const char *const *file_types;
    const uint64_t    *file_sizes;
} oc_mcp_msg;

/* Cut `text` to at most OC_MCP_CUT_AT characters at the last whole line
 * before the limit (or at the limit when it holds no line break). The kept
 * length in bytes, with `*chars` the full length in characters. */
size_t oc_mcp_cut(const char *text, size_t len, size_t limit_chars, size_t *chars);

/* An excerpt of at most OC_MCP_EXCERPT_MAX characters around the first word
 * of `query` found in `text`, ignoring ASCII case; the start when none is. */
size_t oc_mcp_excerpt(const char *text, size_t len, const char *query, char *out, size_t cap);

/* A page of `n` messages, in id order. `newest_first_kept`: when the page
 * must end short of the text budget, keep the newest (a page read backwards)
 * rather than the oldest. Writes `"messages":[...]` and the cursors, and
 * returns how many were kept (at least one when n > 0). `more_older` /
 * `more_newer` say whether the reader holds more beyond the page's ends. A
 * message is cut (`truncated`) unless it is the page's only one. */
size_t oc_mcp_page(oc_mcp_buf *b, const oc_mcp_msg *msgs, size_t n, int newest_first_kept,
                   int more_older, int more_newer, int as_search, const char *query);

/* The per-token budget: a bucket of `burst`, refilled at `per_minute` a
 * minute. Takes `cost` at `now_ms`: 0, or the whole seconds to wait. */
typedef struct { double level; uint64_t at_ms; int primed; } oc_mcp_bucket;
unsigned oc_mcp_bucket_take(oc_mcp_bucket *b, uint64_t now_ms, unsigned cost, unsigned per_minute, unsigned burst);

#endif /* OC_MCP_H */
