/* The MCP authorization server's pure parts (daemon/oauth.c, AUTH.md §8.13):
 * both metadata documents, a registration's and a metadata document's checks,
 * redirect URIs, the authorize and token requests, PKCE S256, the tokens'
 * prefixes, the approval code, the page and the redirect back. */

#include "oauth.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int has(const oc_mcp_buf *b, const char *s) { return b->p && strstr(b->p, s) != NULL; }

static void test_documents(void) {
    char base[300];
    CHECK(oc_oauth_base("ws.example:8443", 15, base, sizeof base) == 0 && strcmp(base, "https://ws.example:8443") == 0);
    CHECK(oc_oauth_base("ws.example/evil", 15, base, sizeof base) != 0);
    CHECK(oc_oauth_base("a\"b", 3, base, sizeof base) != 0);
    CHECK(oc_oauth_base("", 0, base, sizeof base) != 0);

    oc_mcp_buf b = { 0 };
    oc_oauth_resource_metadata(&b, "https://ws.example");
    /* contract: RFC 9728's fields */
    CHECK(has(&b, "\"resource\":\"https://ws.example/mcp\"") && has(&b, "\"authorization_servers\":[\"https://ws.example\"]"));
    oc_mcp_buf_free(&b);
    b = (oc_mcp_buf){ 0 };
    oc_oauth_server_metadata(&b, "https://ws.example");
    /* contract: RFC 8414's fields and the values this server holds to */
    CHECK(has(&b, "\"issuer\":\"https://ws.example\""));
    CHECK(has(&b, "\"authorization_endpoint\":\"https://ws.example/oauth/authorize\""));
    CHECK(has(&b, "\"token_endpoint\":\"https://ws.example/oauth/token\""));
    CHECK(has(&b, "\"registration_endpoint\":\"https://ws.example/oauth/register\""));
    CHECK(has(&b, "\"code_challenge_methods_supported\":[\"S256\"]"));
    CHECK(has(&b, "\"grant_types_supported\":[\"authorization_code\",\"refresh_token\"]"));
    CHECK(has(&b, "\"client_id_metadata_document_supported\":true"));
    oc_mcp_buf_free(&b);
}

static void test_redirects(void) {
    CHECK(oc_oauth_redirect_ok("https://app.example/cb"));
    CHECK(oc_oauth_redirect_ok("http://127.0.0.1:33418/callback"));
    CHECK(oc_oauth_redirect_ok("http://localhost:9/cb"));
    CHECK(oc_oauth_redirect_ok("http://[::1]/cb"));
    CHECK(!oc_oauth_redirect_ok("http://app.example/cb"));            /* plain http off loopback */
    CHECK(!oc_oauth_redirect_ok("http://localhost.evil.example/cb"));
    CHECK(!oc_oauth_redirect_ok("https://app.example/cb#frag"));
    CHECK(!oc_oauth_redirect_ok("https://user@app.example/cb"));
    CHECK(!oc_oauth_redirect_ok("javascript:alert(1)"));
    CHECK(!oc_oauth_redirect_ok("https:///cb"));
    CHECK(!oc_oauth_redirect_ok("https://a b/cb"));

    const char *lines = "https://app.example/cb\nhttp://127.0.0.1:5000/callback";
    CHECK(oc_oauth_uris_has(lines, "https://app.example/cb"));
    CHECK(!oc_oauth_uris_has(lines, "https://app.example/cb2"));
    CHECK(!oc_oauth_uris_has(lines, "https://app.example"));
    /* A loopback URI's port is the client's to pick; its path is not. */
    CHECK(oc_oauth_uris_has(lines, "http://127.0.0.1:61234/callback"));
    CHECK(!oc_oauth_uris_has(lines, "http://127.0.0.1:61234/other"));
    CHECK(!oc_oauth_uris_has(lines, "http://localhost:5000/callback"));

    CHECK(oc_oauth_client_id_is_url("https://client.example/oauth/client.json", 0));
    CHECK(!oc_oauth_client_id_is_url("https://client.example/", 0));
    CHECK(!oc_oauth_client_id_is_url("https://client.example", 0));
    CHECK(!oc_oauth_client_id_is_url("http://127.0.0.1:8/c.json", 0));
    CHECK(oc_oauth_client_id_is_url("http://127.0.0.1:8/c.json", 1));
    CHECK(!oc_oauth_client_id_is_url("occl_abc", 0));
}

static void test_registration(void) {
    oc_oauth_client_meta m;
    char err[160];
    const char *ok = "{\"client_name\":\"Desk Helper\",\"redirect_uris\":[\"http://127.0.0.1:5000/cb\",\"https://d.example/cb\"],"
                     "\"grant_types\":[\"authorization_code\",\"refresh_token\"],\"response_types\":[\"code\"],"
                     "\"token_endpoint_auth_method\":\"none\"}";
    CHECK(oc_oauth_parse_registration(ok, strlen(ok), &m, err, sizeof err) == 0);
    CHECK(strcmp(m.name, "Desk Helper") == 0 && m.n_uris == 2);   /* contract: the input's own name */
    char lines[1200];
    oc_oauth_uris_join(&m, lines, sizeof lines);
    CHECK(strcmp(lines, "http://127.0.0.1:5000/cb\nhttps://d.example/cb") == 0);   /* contract: one to a line */
    oc_mcp_buf b = { 0 };
    oc_oauth_registered(&b, "occl_x", &m, 1700000000);
    CHECK(has(&b, "\"client_id\":\"occl_x\"") && has(&b, "\"token_endpoint_auth_method\":\"none\""));   /* contract: RFC 7591 */
    oc_mcp_buf_free(&b);

    const char *bad[] = {
        "{\"client_name\":\"x\"}",                                                    /* no redirect URIs */
        "{\"redirect_uris\":[]}",
        "{\"redirect_uris\":[\"http://evil.example/cb\"]}",                          /* plain http */
        "{\"redirect_uris\":[\"https://a/1\",\"https://a/2\",\"https://a/3\",\"https://a/4\",\"https://a/5\","
        "\"https://a/6\",\"https://a/7\",\"https://a/8\",\"https://a/9\"]}",          /* nine */
        "{\"redirect_uris\":[\"https://a/cb\"],\"token_endpoint_auth_method\":\"client_secret_basic\"}",
        "{\"redirect_uris\":[\"https://a/cb\"],\"grant_types\":[\"client_credentials\"]}",
        "{\"redirect_uris\":[\"https://a/cb\"],\"response_types\":[\"token\"]}",
        "{\"redirect_uris\":[\"https://a/cb\"],\"client_name\":\"a\\u0007b\"}",       /* a control character */
        "[1]",
        "not json",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        err[0] = '\0';
        CHECK(oc_oauth_parse_registration(bad[i], strlen(bad[i]), &m, err, sizeof err) != 0 && err[0]);
    }
    /* A name too long is refused, not cut. */
    char longname[400];
    snprintf(longname, sizeof longname, "{\"redirect_uris\":[\"https://a/cb\"],\"client_name\":\"%0120d\"}", 0);
    CHECK(oc_oauth_parse_registration(longname, strlen(longname), &m, err, sizeof err) != 0);
    /* No name: one is given. */
    const char *anon = "{\"redirect_uris\":[\"https://a/cb\"]}";
    CHECK(oc_oauth_parse_registration(anon, strlen(anon), &m, err, sizeof err) == 0 && m.name[0]);
}

static void test_document(void) {
    oc_oauth_client_meta m;
    char err[160];
    const char *url = "https://client.example/c.json";
    const char *doc = "{\"client_id\":\"https://client.example/c.json\",\"client_name\":\"Notes AI\","
                      "\"redirect_uris\":[\"https://client.example/cb\"]}";
    CHECK(oc_oauth_parse_document(doc, strlen(doc), url, 0, &m, err, sizeof err) == 0 && m.n_uris == 1);
    /* Its client_id must be its own URL. */
    const char *other = "{\"client_id\":\"https://evil.example/c.json\",\"redirect_uris\":[\"https://client.example/cb\"]}";
    CHECK(oc_oauth_parse_document(other, strlen(other), url, 0, &m, err, sizeof err) != 0);
    const char *none = "{\"redirect_uris\":[\"https://client.example/cb\"]}";
    CHECK(oc_oauth_parse_document(none, strlen(none), url, 0, &m, err, sizeof err) != 0);
    /* Over the size cap, whatever it holds. */
    size_t big = OC_OAUTH_DOC_MAX + 1;
    char *huge = malloc(big + 1);
    if (huge) {
        memset(huge, ' ', big);
        memcpy(huge, doc, strlen(doc));
        huge[big] = '\0';
        CHECK(oc_oauth_parse_document(huge, big, url, 0, &m, err, sizeof err) != 0);
        CHECK(oc_oauth_parse_document(huge, OC_OAUTH_DOC_MAX, url, 0, &m, err, sizeof err) == 0);
        free(huge);
    }
    /* Not a document's URL at all. */
    CHECK(oc_oauth_parse_document(doc, strlen(doc), "http://client.example/c.json", 0, &m, err, sizeof err) != 0);
}

#define CH "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"   /* RFC 7636 appendix B */
#define VER "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"

static void test_authorize(void) {
    oc_oauth_authorize a;
    char err[200];
    const char *res = "https://ws.example/mcp";
    char q[1200];
    snprintf(q, sizeof q, "response_type=code&client_id=occl_1&redirect_uri=http%%3A%%2F%%2F127.0.0.1%%3A5000%%2Fcb"
                          "&code_challenge=" CH "&code_challenge_method=S256&state=xyz+1&resource=https%%3A%%2F%%2Fws.example%%2Fmcp");
    CHECK(oc_oauth_parse_authorize(q, strlen(q), res, &a, err, sizeof err) == 0);
    CHECK(strcmp(a.redirect_uri, "http://127.0.0.1:5000/cb") == 0 && strcmp(a.state, "xyz 1") == 0);   /* contract: decoded */
    CHECK(strcmp(a.client_id, "occl_1") == 0 && !a.request[0]);   /* contract: the input's own value */

    /* Each part missing or wrong is refused on the page. */
    const char *bad[] = {
        "response_type=token&client_id=c&redirect_uri=https%3A%2F%2Fa%2Fcb&code_challenge=" CH "&code_challenge_method=S256&state=s&resource=https%3A%2F%2Fws.example%2Fmcp",
        "response_type=code&redirect_uri=https%3A%2F%2Fa%2Fcb&code_challenge=" CH "&code_challenge_method=S256&state=s&resource=https%3A%2F%2Fws.example%2Fmcp",
        "response_type=code&client_id=c&redirect_uri=http%3A%2F%2Fa%2Fcb&code_challenge=" CH "&code_challenge_method=S256&state=s&resource=https%3A%2F%2Fws.example%2Fmcp",
        "response_type=code&client_id=c&redirect_uri=https%3A%2F%2Fa%2Fcb&code_challenge=" CH "&code_challenge_method=plain&state=s&resource=https%3A%2F%2Fws.example%2Fmcp",
        "response_type=code&client_id=c&redirect_uri=https%3A%2F%2Fa%2Fcb&code_challenge=short&code_challenge_method=S256&state=s&resource=https%3A%2F%2Fws.example%2Fmcp",
        "response_type=code&client_id=c&redirect_uri=https%3A%2F%2Fa%2Fcb&code_challenge=" CH "&code_challenge_method=S256&resource=https%3A%2F%2Fws.example%2Fmcp",
        "response_type=code&client_id=c&redirect_uri=https%3A%2F%2Fa%2Fcb&code_challenge=" CH "&code_challenge_method=S256&state=s&resource=https%3A%2F%2Fother.example%2Fmcp",
        "response_type=code&client_id=c&redirect_uri=https%3A%2F%2Fa%2Fcb&code_challenge=" CH "&code_challenge_method=S256&state=s",
        "response_type=code&response_type=code&client_id=c&redirect_uri=https%3A%2F%2Fa%2Fcb&code_challenge=" CH "&code_challenge_method=S256&state=s&resource=https%3A%2F%2Fws.example%2Fmcp",
        "request=nothandle",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        err[0] = '\0';
        CHECK(oc_oauth_parse_authorize(bad[i], strlen(bad[i]), res, &a, err, sizeof err) != 0 && err[0]);
    }
    /* The page reloading itself carries its handle alone. */
    const char *reload = "request=ocrq_abc&code=BCDFGHJK";
    CHECK(oc_oauth_parse_authorize(reload, strlen(reload), res, &a, err, sizeof err) == 0 && strcmp(a.request, "ocrq_abc") == 0);
    CHECK(strcmp(a.code, "BCDFGHJK") == 0);   /* contract: the code the page shows */
    const char *nocode = "request=ocrq_abc";
    CHECK(oc_oauth_parse_authorize(nocode, strlen(nocode), res, &a, err, sizeof err) != 0);

    char v[16];
    CHECK(oc_oauth_form_get("a=%4", 4, "a", v, sizeof v) == -2);
    CHECK(oc_oauth_form_get("a=%41", 5, "a", v, sizeof v) == 0 && strcmp(v, "A") == 0);
    CHECK(oc_oauth_form_get("a=%00", 5, "a", v, sizeof v) == -2);
    CHECK(oc_oauth_form_get("ab=1", 4, "a", v, sizeof v) == -1);
}

static void test_token_requests(void) {
    oc_oauth_token_req t;
    const char *f = "grant_type=authorization_code&code=ocac_x&code_verifier=" VER "&redirect_uri=https%3A%2F%2Fa%2Fcb&client_id=occl_1";
    CHECK(oc_oauth_parse_token(f, strlen(f), &t) == NULL && t.grant == OC_OAUTH_GRANT_CODE);
    CHECK(strcmp(t.code, "ocac_x") == 0 && strcmp(t.redirect_uri, "https://a/cb") == 0 && !t.resource[0]);   /* contract: decoded */
    const char *r = "grant_type=refresh_token&refresh_token=ocpr_y&client_id=occl_1&resource=https%3A%2F%2Fws%2Fmcp";
    CHECK(oc_oauth_parse_token(r, strlen(r), &t) == NULL && t.grant == OC_OAUTH_GRANT_REFRESH);
    CHECK(strcmp(t.refresh, "ocpr_y") == 0 && strcmp(t.resource, "https://ws/mcp") == 0);   /* contract: decoded */
    const char *e;
    e = oc_oauth_parse_token("grant_type=password&client_id=c", 31, &t);
    CHECK(e && strcmp(e, "unsupported_grant_type") == 0);   /* contract: RFC 6749 §5.2's code */
    e = oc_oauth_parse_token("grant_type=authorization_code&client_id=c&code=x", 48, &t);
    CHECK(e && strcmp(e, "invalid_request") == 0);          /* contract: RFC 6749 §5.2's code */
    e = oc_oauth_parse_token("grant_type=refresh_token&refresh_token=x", 40, &t);
    CHECK(e && strcmp(e, "invalid_request") == 0);          /* contract: RFC 6749 §5.2's code */
}

static void test_pkce_and_tokens(void) {
    CHECK(oc_oauth_pkce_ok(VER, CH));
    CHECK(!oc_oauth_pkce_ok(VER "x", CH));
    CHECK(!oc_oauth_pkce_ok("short", CH));
    char longv[200];
    memset(longv, 'a', 129); longv[129] = '\0';
    CHECK(!oc_oauth_pkce_ok(longv, CH));
    CHECK(!oc_oauth_pkce_ok("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjX!", CH));

    char a[OC_OAUTH_TOKEN_MAX], b[OC_OAUTH_TOKEN_MAX];
    CHECK(oc_oauth_mint(OC_OAUTH_ACCESS_PREFIX, a, sizeof a) == 0 && oc_oauth_mint(OC_OAUTH_ACCESS_PREFIX, b, sizeof b) == 0);
    CHECK(strlen(a) == 5 + 43 && strncmp(a, "ocpa_", 5) == 0 && strcmp(a, b) != 0);   /* contract: the prefix a scanner knows */
    CHECK(oc_oauth_token_of(a, strlen(a)) == OC_OAUTH_TOKEN_PERSON);
    CHECK(oc_oauth_token_of("ocpt_AAAA", 9) == OC_OAUTH_TOKEN_AGENT);
    CHECK(oc_oauth_token_of("ocpr_AAAA", 9) == OC_OAUTH_TOKEN_OTHER);   /* a refresh token is no bearer */
    CHECK(oc_oauth_token_of("ocac_AAAA", 9) == OC_OAUTH_TOKEN_OTHER);
    uint8_t h1[32], h2[32];
    CHECK(oc_oauth_hash(a, strlen(a), h1) == 0 && oc_oauth_hash(b, strlen(b), h2) == 0 && memcmp(h1, h2, 32) != 0);

    char code[OC_OAUTH_CODE_LEN + 1], norm[OC_OAUTH_CODE_LEN + 1];
    for (int i = 0; i < 50; i++) {
        CHECK(oc_oauth_approval_code(code) == 0 && strlen(code) == OC_OAUTH_CODE_LEN);
        CHECK(strspn(code, "BCDFGHJKLMNPQRSTVWXZ") == OC_OAUTH_CODE_LEN);
    }
    CHECK(oc_oauth_code_normalize("bcdf-ghjk", norm) == 0 && strcmp(norm, "BCDFGHJK") == 0);   /* contract: normalised */
    CHECK(oc_oauth_code_normalize(" BCDF GHJK ", norm) == 0);
    CHECK(oc_oauth_code_normalize("BCDF-GHJ", norm) != 0);
    CHECK(oc_oauth_code_normalize("BCDF-GHJKL", norm) != 0);
    CHECK(oc_oauth_code_normalize("ABCD-GHJK", norm) != 0);   /* a vowel is no code's */
    CHECK(oc_oauth_lifetime_ok(0) && oc_oauth_lifetime_ok(30) && oc_oauth_lifetime_ok(90) && oc_oauth_lifetime_ok(365));
    CHECK(!oc_oauth_lifetime_ok(7) && !oc_oauth_lifetime_ok(366));
}

static void test_page_and_redirect(void) {
    oc_mcp_buf b = { 0 };
    oc_oauth_page(&b, OC_OAUTH_PAGE_PENDING, "<b>Bot</b>", "BCDFGHJK", "/oauth/authorize?request=ocrq_1", NULL);
    CHECK(has(&b, "BCDF-GHJK") && has(&b, "&lt;b&gt;Bot&lt;/b&gt;") && !has(&b, "<b>Bot"));
    CHECK(has(&b, "http-equiv=\"refresh\" content=\"3;url=/oauth/authorize?request=ocrq_1\""));
    CHECK(!has(&b, "<script") && !has(&b, "<form") && !has(&b, "password"));
    oc_mcp_buf_free(&b);
    b = (oc_mcp_buf){ 0 };
    oc_oauth_page(&b, OC_OAUTH_PAGE_EXPIRED, NULL, NULL, NULL, NULL);
    CHECK(has(&b, "expired") && !has(&b, "refresh"));
    oc_mcp_buf_free(&b);

    b = (oc_mcp_buf){ 0 };
    oc_oauth_redirect(&b, "https://a.example/cb?x=1", "ocac_Z", NULL, "s t&u", "https://ws.example");
    CHECK(b.p && strcmp(b.p, "https://a.example/cb?x=1&code=ocac_Z&state=s%20t%26u&iss=https%3A%2F%2Fws.example") == 0);   /* contract: RFC 6749 §4.1.2 */
    oc_mcp_buf_free(&b);
    b = (oc_mcp_buf){ 0 };
    oc_oauth_redirect(&b, "http://127.0.0.1:9/cb", NULL, "access_denied", "s", NULL);
    CHECK(b.p && strcmp(b.p, "http://127.0.0.1:9/cb?error=access_denied&state=s") == 0);   /* contract: RFC 6749 §4.1.2.1 */
    oc_mcp_buf_free(&b);

    b = (oc_mcp_buf){ 0 };
    oc_oauth_token_json(&b, "ocpa_A", "ocpr_B", 3600);
    CHECK(has(&b, "\"token_type\":\"Bearer\"") && has(&b, "\"expires_in\":3600"));   /* contract: RFC 6749 §5.1 */
    oc_mcp_buf_free(&b);
}

int run_oauth_tests(void) {
    printf("test_oauth: metadata, client identification, authorize and token requests, PKCE, tokens, the page\n");
    test_documents();
    test_redirects();
    test_registration();
    test_document();
    test_authorize();
    test_token_requests();
    test_pkce_and_tokens();
    test_page_and_redirect();
    return failures;
}
