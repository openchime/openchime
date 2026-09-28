/* daemon/unfurl.c — the pure halves of the link-unfurl fetcher (REQ-222,
 * ARCH-105): the SSRF gate's per-address verdict, and the HTML title/
 * description scan. The network half is deliberately not driven here — the
 * gate is the control that makes it safe, so the gate is what gets the
 * exhaustive table. */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "unfurl.h"

static int v4(const char *dotted) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    inet_pton(AF_INET, dotted, &sa.sin_addr);
    return oc_unfurl_addr_public((const struct sockaddr *)&sa);
}

static int v6(const char *text) {
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    inet_pton(AF_INET6, text, &sa.sin6_addr);
    return oc_unfurl_addr_public((const struct sockaddr *)&sa);
}

static int test_ssrf_gate(void) {
    int failures = 0;

    /* Public addresses pass. */
    CHECK(v4("93.184.216.34"));
    CHECK(v4("8.8.8.8"));
    CHECK(v4("172.15.0.1"));      /* just below 172.16/12 */
    CHECK(v4("172.32.0.1"));      /* just above it        */
    CHECK(v4("100.63.255.255"));  /* just below CGNAT     */
    CHECK(v4("100.128.0.1"));     /* just above it        */
    CHECK(v6("2606:4700::1111"));

    /* The non-routable set is refused, both families. */
    CHECK(!v4("0.0.0.0"));
    CHECK(!v4("10.0.0.1"));
    CHECK(!v4("100.64.0.1"));     /* CGNAT              */
    CHECK(!v4("127.0.0.1"));
    CHECK(!v4("169.254.1.1"));    /* link-local         */
    CHECK(!v4("172.16.0.1"));
    CHECK(!v4("172.31.255.255"));
    CHECK(!v4("192.0.0.1"));
    CHECK(!v4("192.0.2.7"));      /* documentation      */
    CHECK(!v4("192.168.1.1"));
    CHECK(!v4("198.18.0.1"));     /* benchmark          */
    CHECK(!v4("198.19.255.255"));
    CHECK(!v4("198.51.100.1"));   /* documentation      */
    CHECK(!v4("203.0.113.9"));    /* documentation      */
    CHECK(!v4("224.0.0.1"));      /* multicast          */
    CHECK(!v4("255.255.255.255"));

    CHECK(!v6("::"));
    CHECK(!v6("::1"));
    CHECK(!v6("::ffff:10.0.0.1"));    /* v4-mapped private judged as its v4 */
    CHECK(!v6("::ffff:127.0.0.1"));
    CHECK(v6("::ffff:8.8.8.8"));      /* v4-mapped public passes as its v4  */
    CHECK(!v6("64:ff9b::1.2.3.4"));   /* NAT64: an indirection, refused     */
    CHECK(!v6("fc00::1"));            /* ULA                                */
    CHECK(!v6("fd12:3456::1"));
    CHECK(!v6("fe80::1"));            /* link-local                         */
    CHECK(!v6("ff02::1"));            /* multicast                          */
    CHECK(!v6("2001:db8::1"));        /* documentation                      */
    /* The IPv6 special-purpose blocks that carry, tunnel or reserve. */
    CHECK(!v6("::7f00:1"));           /* ::/96 IPv4-compatible 127.0.0.1    */
    CHECK(!v6("::a00:1"));            /* ::/96 IPv4-compatible 10.0.0.1     */
    CHECK(v6("::808:808"));           /* ::/96 around a public v4: its v4   */
    CHECK(!v6("::ffff:0:a00:1"));     /* SIIT, IPv4-translated: refused     */
    CHECK(!v6("2002:7f00:1::1"));     /* 6to4 around 127.0.0.1              */
    CHECK(!v6("2002:c0a8:101::1"));   /* 6to4 around 192.168.1.1            */
    CHECK(v6("2002:808:808::1"));     /* 6to4 around a public v4            */
    CHECK(!v6("2001::1"));            /* Teredo, a tunnel                   */
    CHECK(!v6("2001:2::1"));          /* benchmarking                       */
    CHECK(!v6("2001:10::1"));         /* ORCHID                             */
    CHECK(!v6("2001:20::1"));         /* ORCHIDv2                           */
    CHECK(!v6("2001:1ff::1"));        /* the rest of 2001::/23              */
    CHECK(v6("2001:200::1"));         /* just above it: a real allocation   */
    CHECK(!v6("100::1"));             /* discard-only                       */
    CHECK(v6("100:0:0:1::1"));        /* just past the /64                  */
    CHECK(!v6("3fff::1"));            /* documentation (RFC 9637)           */
    CHECK(!v6("5f00::1"));            /* SRv6 SIDs                          */
    CHECK(!v6("64:ff9b:1::1"));       /* local-use NAT64                    */

    /* A link to an IPv6 address is taken (it used to be refused outright) and
     * then meets the gate like any resolved address. */
    {
        char host[256], port[16];
        CHECK(oc_unfurl_url_target("https://[2001:db8::1]:8443/x", host, sizeof host, port, sizeof port) == 0 &&
              strcmp(host, "2001:db8::1") == 0 && strcmp(port, "8443") == 0);
        CHECK(!v6(host));                                   /* documentation space */
        CHECK(oc_unfurl_url_target("https://[2606:4700::1111]/", host, sizeof host, port, sizeof port) == 0 &&
              strcmp(host, "2606:4700::1111") == 0 && strcmp(port, "443") == 0);
        CHECK(v6(host));                                    /* public: through the gate */
        CHECK(oc_unfurl_url_target("http://example.com:8080/a?b", host, sizeof host, port, sizeof port) == 0 &&
              strcmp(host, "example.com") == 0 && strcmp(port, "8080") == 0);
        CHECK(oc_unfurl_url_target("https://2001:db8::1/", host, sizeof host, port, sizeof port) != 0);
        CHECK(oc_unfurl_url_target("https://[2001:db8::1/", host, sizeof host, port, sizeof port) != 0);
        CHECK(oc_unfurl_url_target("ftp://example.com/", host, sizeof host, port, sizeof port) != 0);
    }

    /* A family the gate cannot judge is one it does not pass. */
    {
        struct sockaddr sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_family = AF_UNIX;
        CHECK(!oc_unfurl_addr_public(&sa));
        CHECK(!oc_unfurl_addr_public(NULL));
    }
    return failures;
}

static int extract(const char *html, char *t, size_t tc, char *d, size_t dc) {
    return oc_unfurl_extract_html(html, strlen(html), t, tc, d, dc);
}

static int test_html_extract(void) {
    int failures = 0;
    char t[256], d[512];

    /* og: wins over the document title — it is what the author wrote for
     * exactly this use. */
    CHECK(extract("<html><head>"
                  "<meta property=\"og:title\" content=\"OG Title\">"
                  "<meta property=\"og:description\" content=\"OG Desc\">"
                  "<title>Doc Title</title></head></html>", t, sizeof t, d, sizeof d) == 0);
    CHECK(strcmp(t, "OG Title") == 0 && strcmp(d, "OG Desc") == 0);

    /* Fallbacks: <title> and <meta name="description">. */
    CHECK(extract("<head><title>Plain Title</title>"
                  "<meta name=\"description\" content=\"Plain Desc\"></head>",
                  t, sizeof t, d, sizeof d) == 0);
    CHECK(strcmp(t, "Plain Title") == 0 && strcmp(d, "Plain Desc") == 0);

    /* Case-blind tags, single quotes, attribute order, self-closing. */
    CHECK(extract("<META CONTENT='X marks &amp; spots' PROPERTY='og:title'/>",
                  t, sizeof t, d, sizeof d) == 0);
    CHECK(strcmp(t, "X marks & spots") == 0 && d[0] == '\0');

    /* Entities and whitespace collapse inside <title>. */
    CHECK(extract("<title>\n  A &lt;B&gt; &#39;C&#x27;   D\t</title>",
                  t, sizeof t, d, sizeof d) == 0);
    CHECK(strcmp(t, "A <B> 'C' D") == 0);

    /* No title anywhere: -1, and the description alone does not rescue it. */
    CHECK(extract("<meta name=\"description\" content=\"only this\">",
                  t, sizeof t, d, sizeof d) == -1);
    CHECK(extract("<p>nothing here</p>", t, sizeof t, d, sizeof d) == -1);
    CHECK(oc_unfurl_extract_html(NULL, 0, t, sizeof t, d, sizeof d) == -1);

    /* Truncation lands on a UTF-8 boundary, never mid-sequence: the cap falls
     * after the LEAD byte of the third e-acute, and the whole sequence goes. */
    {
        char small[8];
        CHECK(extract("<title>ab\xC3\xA9\xC3\xA9\xC3\xA9zzz</title>",   /* ab + e-acute x3 */
                      small, sizeof small, d, sizeof d) == 0);
        CHECK(strcmp(small, "ab\xC3\xA9\xC3\xA9") == 0);
    }
    return failures;
}

/* Block pages served with a 200 (issue phrasing: microsoft.com answering
 * "your request has been blocked") must produce NO unfurl. */
static int test_block_pages(void) {
    int failures = 0;

    CHECK(oc_unfurl_blocked("Your request has been blocked", ""));
    CHECK(oc_unfurl_blocked("Access Denied", ""));
    CHECK(oc_unfurl_blocked("Attention Required! | Cloudflare", ""));
    CHECK(oc_unfurl_blocked("Just a moment...", ""));
    CHECK(oc_unfurl_blocked("Robot or human?", "Verify you are human to continue"));
    CHECK(oc_unfurl_blocked("Page not found - Example", ""));
    CHECK(oc_unfurl_blocked("Error", "Access to this page has been denied."));
    CHECK(oc_unfurl_blocked("503 Service Unavailable", ""));

    /* Real titles pass, including ones ABOUT blocking — the list is phrases,
     * not words. */
    CHECK(!oc_unfurl_blocked("Example Domain", ""));
    CHECK(!oc_unfurl_blocked("How ad blocking works", "A primer on request filtering"));
    CHECK(!oc_unfurl_blocked("The Momentum of a Moment", ""));
    CHECK(!oc_unfurl_blocked("", ""));
    CHECK(!oc_unfurl_blocked(NULL, NULL));

    return failures;
}

int run_unfurl_tests(void) {
    int failures = 0;
    printf("unfurl: SSRF gate\n");
    failures += test_ssrf_gate();
    printf("unfurl: HTML extraction\n");
    failures += test_html_extract();
    printf("unfurl: block pages\n");
    failures += test_block_pages();
    return failures;
}
