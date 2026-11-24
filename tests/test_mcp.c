/* The MCP surface's pure parts (daemon/mcp.c, docs/details/MCP.md): a request's
 * checks, the tool schemas, a page bounded by count and by text, a message cut
 * at a whole line, a search excerpt, and the per-token budget on a clock the
 * test moves. */

#include "mcp.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V OC_MCP_REVISION

static oc_mcp_headers hdrs(const char *method, const char *name) {
    oc_mcp_headers h;
    memset(&h, 0, sizeof h);
    h.version = V; h.version_len = strlen(V);
    h.method = method; h.method_len = method ? strlen(method) : 0;
    h.name = name; h.name_len = name ? strlen(name) : 0;
    h.host = "ws.example"; h.host_len = 10;
    return h;
}

static int parse(const char *body, const oc_mcp_headers *h, oc_mcp_call *c, oc_mcp_buf *err) {
    err->len = 0;
    return oc_mcp_parse(body, strlen(body), h, c, err);
}

#define META "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"" V "\"}"

static void test_requests(void) {
    oc_mcp_call c;
    oc_mcp_buf err = { 0 };
    oc_mcp_headers h = hdrs("tools/list", NULL);

    /* Served. */
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\",\"params\":{" META "}}", &h, &c, &err) == 0);
    CHECK(c.method == OC_MCP_TOOLS_LIST && strcmp(c.id, "7") == 0);
    oc_mcp_call_free(&c);

    /* Another revision, none, and the earlier era's handshake: unsupported. */
    oc_mcp_headers old = h; old.version = "2025-06-18"; old.version_len = 10;
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{\"_meta\":"
                "{\"io.modelcontextprotocol/protocolVersion\":\"2025-06-18\"}}}", &old, &c, &err) == 400);
    CHECK(err.p && strstr(err.p, "UnsupportedProtocolVersionError") && strstr(err.p, V));
    oc_mcp_call_free(&c);
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}", &h, &c, &err) == 400);
    oc_mcp_call_free(&c);
    oc_mcp_headers init = hdrs("initialize", NULL);
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}", &init, &c, &err) == 400);
    CHECK(err.p && strstr(err.p, "UnsupportedProtocolVersionError"));
    oc_mcp_call_free(&c);

    /* Headers that disagree with the body. */
    oc_mcp_headers wrong = hdrs("tools/call", NULL);
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{" META "}}", &wrong, &c, &err) == 400);
    CHECK(err.p && strstr(err.p, "-32020"));
    oc_mcp_call_free(&c);
    oc_mcp_headers noname = hdrs("tools/call", NULL);
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"search\","
                "\"arguments\":{\"query\":\"x\"}," META "}}", &noname, &c, &err) == 400);
    CHECK(err.p && strstr(err.p, "-32020"));
    oc_mcp_call_free(&c);
    oc_mcp_headers vmis = h; vmis.version = "2099-01-01"; vmis.version_len = 10;
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{" META "}}", &vmis, &c, &err) == 400);
    CHECK(err.p && strstr(err.p, "-32020"));
    oc_mcp_call_free(&c);

    /* Another site's page is refused; the workspace's own origin is not. */
    oc_mcp_headers fo = h; fo.origin = "https://evil.example"; fo.origin_len = 20;
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{" META "}}", &fo, &c, &err) == 403);
    oc_mcp_call_free(&c);
    oc_mcp_headers so = h; so.origin = "https://ws.example"; so.origin_len = 18;
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{" META "}}", &so, &c, &err) == 0);
    oc_mcp_call_free(&c);

    /* Malformed, and a method it does not have. */
    CHECK(parse("{not json", &h, &c, &err) == 400);
    oc_mcp_call_free(&c);
    oc_mcp_headers sl = hdrs("subscriptions/listen", NULL);
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"subscriptions/listen\",\"params\":{" META "}}", &sl, &c, &err) == 404);
    CHECK(err.p && strstr(err.p, "-32601"));
    oc_mcp_call_free(&c);

    /* A tool call's arguments, read; and wrong ones, said. */
    oc_mcp_headers rh = hdrs("tools/call", "read_history");
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"tools/call\",\"params\":{\"name\":\"read_history\","
                "\"arguments\":{\"channel\":\"#general\",\"before\":90,\"since\":\"2026-11-01T00:00:00Z\"}," META "}}",
                &rh, &c, &err) == 0);
    CHECK(c.tool == OC_MCP_READ_HISTORY && !c.arg_error[0] && strcmp(c.channel, "#general") == 0);
    CHECK(c.has_before && c.before == 90 && c.has_since && c.since_ms == 1793491200000ull &&
          c.limit == OC_MCP_HISTORY_DEFAULT && strcmp(c.id, "\"a\"") == 0);
    oc_mcp_call_free(&c);
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"read_history\","
                "\"arguments\":{\"channel\":7,\"before\":9,\"after\":3}," META "}}", &rh, &c, &err) == 0);
    CHECK(c.arg_error[0] != '\0' && strcmp(c.channel, "7") == 0);
    oc_mcp_call_free(&c);
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"read_history\","
                "\"arguments\":{\"channel\":\"#g\",\"limit\":201}," META "}}", &rh, &c, &err) == 0);
    CHECK(c.arg_error[0] != '\0');
    oc_mcp_call_free(&c);
    oc_mcp_headers ph = hdrs("tools/call", "post");
    CHECK(parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"post\","
                "\"arguments\":{\"channel\":\"#g\",\"text\":\"hi\\nthere\",\"key\":\"k1\"}," META "}}", &ph, &c, &err) == 0);
    CHECK(!c.arg_error[0] && c.text_len == 8 && memcmp(c.text, "hi\nthere", 8) == 0 && strcmp(c.key, "k1") == 0);
    oc_mcp_call_free(&c);
    oc_mcp_buf_free(&err);
}

static void test_schemas(void) {
    const char *names[2] = { "#general", "#ops" };
    oc_mcp_buf b = { 0 };
    oc_mcp_tools_list(&b, "3", "1.0", names, 2);
    CHECK(b.p != NULL);
    if (!b.p) return;
    const char *rh = strstr(b.p, "\"read_history\""), *se = strstr(b.p, "\"search\""), *po = strstr(b.p, "\"post\"");
    CHECK(rh && se && po && rh < se && se < po);                 /* in that order */
    CHECK(strstr(b.p, "\"enum\":[\"#general\",\"#ops\"]") != NULL);
    CHECK(strstr(b.p, "\"cacheScope\":\"private\",\"ttlMs\":60000") != NULL);
    CHECK(strstr(b.p, "\"resultType\":\"complete\"") != NULL);
    oc_mcp_buf_free(&b);
    oc_mcp_discover(&b, "1", "1.0");
    CHECK(b.p && strstr(b.p, "\"tools\"") && strstr(b.p, V));
    oc_mcp_buf_free(&b);
}

static char *repeat(char ch, size_t n) {
    char *s = malloc(n + 1);
    memset(s, ch, n);
    s[n] = '\0';
    return s;
}

static void test_cut_and_excerpt(void) {
    /* Under the limit: whole. Over: at the last whole line, with the length. */
    size_t chars = 0;
    CHECK(oc_mcp_cut("short", 5, OC_MCP_CUT_AT, &chars) == 5 && chars == 5);
    char *big = repeat('a', 9000);
    big[5000] = '\n';
    CHECK(oc_mcp_cut(big, 9000, OC_MCP_CUT_AT, &chars) == 5000 && chars == 9000);
    big[5000] = 'a';
    CHECK(oc_mcp_cut(big, 9000, OC_MCP_CUT_AT, &chars) == OC_MCP_CUT_AT);   /* no line: at the limit */
    free(big);
    /* Characters, not bytes: two-byte letters. */
    char *wide = malloc(2 * 8100 + 1);
    for (size_t i = 0; i < 8100; i++) { wide[2 * i] = (char)0xC3; wide[2 * i + 1] = (char)0xA9; }
    wide[2 * 8100] = '\0';
    CHECK(oc_mcp_cut(wide, 2 * 8100, OC_MCP_CUT_AT, &chars) == 2 * OC_MCP_CUT_AT && chars == 8100);
    free(wide);

    char ex[2048];
    char *t = repeat('x', 2000);
    memcpy(t + 1500, "Deploy", 6);
    size_t n = oc_mcp_excerpt(t, 2000, "deploy failed", ex, sizeof ex);
    CHECK(n > 0 && n <= OC_MCP_EXCERPT_MAX && strstr(ex, "Deploy") != NULL);
    n = oc_mcp_excerpt(t, 2000, "absent", ex, sizeof ex);
    CHECK(n == OC_MCP_EXCERPT_MAX && ex[0] == 'x');                         /* the start */
    free(t);
}

static void test_page(void) {
    /* Twenty messages of 5 000 characters: a 64 KiB page holds thirteen. */
    oc_mcp_msg m[20];
    char *body = repeat('b', 5000);
    memset(m, 0, sizeof m);
    for (int i = 0; i < 20; i++) {
        m[i].id = (uint64_t)(100 + i); m[i].author = "ada"; m[i].handle = "ada"; m[i].author_kind = "person";
        m[i].at_ms = 1000; m[i].text = body; m[i].text_len = 5000;
    }
    oc_mcp_buf b = { 0 };
    size_t kept = oc_mcp_page(&b, m, 20, 1, 0, 0, 0, NULL);
    CHECK(kept == 13);
    /* Read backwards, the newest are kept, and the older cursor continues. */
    CHECK(b.p && strstr(b.p, "\"id\":119") && !strstr(b.p, "\"id\":106,") && strstr(b.p, "\"older\":107"));
    oc_mcp_buf_free(&b);
    kept = oc_mcp_page(&b, m, 20, 0, 0, 0, 0, NULL);
    CHECK(kept == 13 && b.p && strstr(b.p, "\"id\":100") && strstr(b.p, "\"newer\":112"));
    oc_mcp_buf_free(&b);
    /* Under the text budget, the count the reader was asked for is the page. */
    kept = oc_mcp_page(&b, m, 5, 1, 1, 0, 0, NULL);
    CHECK(kept == 5 && b.p && strstr(b.p, "\"older\":100"));
    oc_mcp_buf_free(&b);
    /* At the end in both directions: no cursors. */
    kept = oc_mcp_page(&b, m, 2, 1, 0, 0, 0, NULL);
    CHECK(kept == 2 && b.p && !strstr(b.p, "\"older\"") && !strstr(b.p, "\"newer\""));
    oc_mcp_buf_free(&b);
    /* A long message is cut on a page of several, and whole on a page of one. */
    char *longt = repeat('l', 9000);
    oc_mcp_msg two[2] = { m[0], m[1] };
    two[1].text = longt; two[1].text_len = 9000;
    oc_mcp_page(&b, two, 2, 1, 0, 0, 0, NULL);
    CHECK(b.p && strstr(b.p, "\"truncated\":{\"length\":9000}"));
    oc_mcp_buf_free(&b);
    oc_mcp_page(&b, &two[1], 1, 1, 0, 0, 0, NULL);
    CHECK(b.p && !strstr(b.p, "\"truncated\""));
    oc_mcp_buf_free(&b);
    free(longt);
    free(body);
}

static void test_bucket(void) {
    oc_mcp_bucket bk = { 0 };
    uint64_t now = 1000000;
    for (int i = 0; i < 20; i++) CHECK(oc_mcp_bucket_take(&bk, now, 1, 60, 20) == 0);
    CHECK(oc_mcp_bucket_take(&bk, now, 1, 60, 20) == 1);                       /* empty: a second to wait */
    now += 1000;                                                               /* a second refills one */
    CHECK(oc_mcp_bucket_take(&bk, now, 1, 60, 20) == 0);
    CHECK(oc_mcp_bucket_take(&bk, now, 1, 60, 20) != 0);
    now += 3000;
    CHECK(oc_mcp_bucket_take(&bk, now, OC_MCP_SEARCH_COST, 60, 20) == 0);     /* a search takes three */
    CHECK(oc_mcp_bucket_take(&bk, now, 1, 60, 20) != 0);
    now += 3600000;                                                            /* never past the burst */
    for (int i = 0; i < 20; i++) CHECK(oc_mcp_bucket_take(&bk, now, 1, 60, 20) == 0);
    CHECK(oc_mcp_bucket_take(&bk, now, 1, 60, 20) != 0);
}

int run_mcp_tests(void) {
    printf("test_mcp: the request checks, the tool schemas, cutting and excerpts, a page by count and by text, "
           "the per-token budget\n");
    test_requests();
    test_schemas();
    test_cut_and_excerpt();
    test_page();
    test_bucket();
    return failures;
}
