/*
 * OpenChime client — workspace resolution. See resolve.h.
 */

#include "resolve.h"

#include "wellknown.h"

#include <stdlib.h>   /* getenv */
#include "sock.h"      /* oc_sock_startup: getaddrinfo needs WSAStartup on Windows */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windns.h>
#elif defined(__EMSCRIPTEN__)
   /* A browser has no resolver to ask for SRV: the workspace is reached at the
    * address the page was given (CLIENT.md §4). */
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <sys/socket.h>
#else
#  include <arpa/inet.h>
#  include <resolv.h>
#  include <arpa/nameser.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <sys/socket.h>
#endif

#include <stdio.h>
#include <string.h>

const char *oc_default_suffix(void) {
    const char *env = getenv("OPENCHIME_SUFFIX");
    return (env && *env) ? env : OC_SERVICE_SUFFIX;
}

/* Discovery attempts, for the tests' proof that a literal makes none (resolve.h). */
static unsigned g_srv_attempts, g_dns_attempts, g_wk_attempts;

void oc_resolve_counts(unsigned *srv, unsigned *dns, unsigned *wellknown) {
    if (srv) *srv = g_srv_attempts;
    if (dns) *dns = g_dns_attempts;
    if (wellknown) *wellknown = g_wk_attempts;
}

/* Split what was typed into its host and port, after a scheme and before a
 * path: `host`, `host:port`, `[v6]`, `[v6]:port`, or a bare IPv6 address (two or
 * more colons, which cannot carry a port). The host comes back without brackets.
 * `*port` is the port typed, 0 for none, or -1 for one that is not a port.
 * `*bracketed` says brackets were used, which only an IPv6 address may wear.
 * 0, or -1 when there is no host, a bracket is unclosed or followed by junk, or
 * the host does not fit. */
static int ws_split(const char *workspace, char *host, size_t hcap, long *port, int *bracketed) {
    *port = 0; *bracketed = 0;
    if (!workspace || hcap == 0) return -1;
    const char *s = workspace;
    while (*s == ' ') s++;
    const char *scheme = strstr(s, "://");
    if (scheme) s = scheme + 3;
    const char *end = s + strcspn(s, "/");
    const char *h = s, *he = end, *pp = NULL;
    if (*s == '[') {
        const char *rb = memchr(s, ']', (size_t)(end - s));
        if (!rb) return -1;
        h = s + 1; he = rb;
        *bracketed = 1;
        if (rb + 1 < end) {
            if (rb[1] != ':') return -1;
            pp = rb + 2;
        }
    } else {
        const char *c = memchr(s, ':', (size_t)(end - s));
        if (c && !memchr(c + 1, ':', (size_t)(end - c - 1))) { he = c; pp = c + 1; }
        /* two or more colons: a bare IPv6 address, the whole of it the host */
    }
    size_t hn = (size_t)(he - h);
    if (hn == 0 || hn >= hcap) return -1;
    memcpy(host, h, hn); host[hn] = '\0';
    if (pp) {
        long v = 0; const char *q = pp;
        for (; q < end && *q >= '0' && *q <= '9' && v <= 65535; q++) v = v * 10 + (*q - '0');
        *port = (q == end && q > pp && v >= 1 && v <= 65535) ? v : -1;
    }
    return 0;
}

/* Is `host` an address literal? Its canonical text (inet_ntop, the zone kept
 * after `%`) in `canon`, and whether it is IPv6. An address is one spelling, so
 * one address is one key for its session and pin. */
static int literal_of(const char *host, char *canon, size_t cap, int *v6) {
    unsigned char b[16];
    char a[128];
    const char *zone = strchr(host, '%');
    size_t an = zone ? (size_t)(zone - host) : strlen(host);
    if (an == 0 || an >= sizeof a) return 0;
    memcpy(a, host, an); a[an] = '\0';
    char txt[64];
    if (!zone && inet_pton(AF_INET, a, b) == 1) {
        if (!inet_ntop(AF_INET, b, txt, sizeof txt)) return 0;
        *v6 = 0;
    } else if (inet_pton(AF_INET6, a, b) == 1) {
        if (!inet_ntop(AF_INET6, b, txt, sizeof txt)) return 0;
        *v6 = 1;
    } else {
        return 0;
    }
    if (zone && (!zone[1] || strpbrk(zone + 1, "[]/:"))) return 0;
    int n = snprintf(canon, cap, "%s%s", txt, zone ? zone : "");
    return n > 0 && (size_t)n < cap;
}

int oc_resolve_domain(const char *workspace, const char *suffix, char *out, size_t cap) {
    if (!workspace || !out || cap == 0) return -1;
    char host[256];
    long port; int bracketed;
    if (ws_split(workspace, host, sizeof host, &port, &bracketed) != 0) return -1;

    /* An address is its own domain, never a name to suffix: IPv6 in brackets,
     * so a port after it cannot be mistaken for part of it. */
    char canon[128]; int v6 = 0;
    if (literal_of(host, canon, sizeof canon, &v6)) {
        if (bracketed && !v6) return -1;         /* brackets are IPv6's alone */
        int n = v6 ? snprintf(out, cap, "[%s]", canon) : snprintf(out, cap, "%s", canon);
        return (n < 0 || (size_t)n >= cap) ? -1 : 0;
    }
    if (bracketed || strchr(host, ':')) return -1;   /* not an address, and a name has no colon */

    /* `localhost` is a host, not an org shorthand: appending the service suffix
     * to it produced "localhost.openchime.io", which resolves nowhere, so the one
     * address every developer reaches for was the one that could not be used.
     * A trailing dot is the same name, absolutely qualified. */
    size_t hlen = strlen(host);
    if (hlen && host[hlen - 1] == '.') host[--hlen] = '\0';
    int loopback = 1;
    static const char LH[] = "localhost";
    if (hlen != sizeof LH - 1) loopback = 0;
    else for (size_t i = 0; i < hlen && loopback; i++) {
        char c = host[i] >= 'A' && host[i] <= 'Z' ? (char)(host[i] - 'A' + 'a') : host[i];
        if (c != LH[i]) loopback = 0;
    }

    if (loopback) {
        /* One spelling, because this name becomes the key a session token and a
         * trusted fingerprint are stored under: "LocalHost" and "localhost" are the same
         * host, and keeping both would be two entries for one workspace. */
        if ((size_t)snprintf(out, cap, "localhost") >= cap) return -1;
    } else if (!strchr(host, '.') && suffix && *suffix) {   /* bare name -> append suffix */
        if ((size_t)snprintf(out, cap, "%s.%s", host, suffix) >= cap) return -1;
    } else {
        if ((size_t)snprintf(out, cap, "%s", host) >= cap) return -1;
    }
    return 0;
}

int oc_workspace_takes_suffix(const char *typed, const char *suffix) {
    if (!suffix || !*suffix) return 0;
    if (!typed || !*typed) return 1;
    /* Anything that names more than a label is taken as typed. */
    if (strpbrk(typed, ".:/[]")) return 0;
    /* And the rest is exactly what resolution would suffix: a bare name, but not
     * `localhost`. */
    char d[256], bare[256];
    if (oc_resolve_domain(typed, suffix, d, sizeof d) != 0 ||
        oc_resolve_domain(typed, NULL, bare, sizeof bare) != 0) return 0;
    return strcmp(d, bare) != 0;
}

int oc_hostport(const char *host, int port, char *out, size_t cap) {
    if (!host || !out || cap == 0) return -1;
    int n = strchr(host, ':') ? snprintf(out, cap, "[%s]:%d", host, port)
                              : snprintf(out, cap, "%s:%d", host, port);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int oc_addr_is_loopback(const char *host) {
    if (!host) return 0;
    char a[128];
    size_t n = strlen(host);
    if (n >= 2 && host[0] == '[' && host[n - 1] == ']') { host++; n -= 2; }
    if (n == 0 || n >= sizeof a) return 0;
    memcpy(a, host, n); a[n] = '\0';
    static const char LH[] = "localhost";
    if (n == sizeof LH - 1) {
        size_t i = 0;
        for (; i < n; i++) {
            char c = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] - 'A' + 'a') : a[i];
            if (c != LH[i]) break;
        }
        if (i == n) return 1;
    }
    unsigned char b[16];
    if (inet_pton(AF_INET, a, b) == 1) return b[0] == 127;
    if (inet_pton(AF_INET6, a, b) == 1) {
        static const unsigned char one[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
        static const unsigned char mapped[12] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
        if (memcmp(b, one, 16) == 0) return 1;
        if (memcmp(b, mapped, 12) == 0) return b[12] == 127;
    }
    return 0;
}

int oc_sni_name(const char *name, char *out, size_t cap) {
    if (!name || !out || cap == 0) return 0;
    out[0] = '\0';
    char host[256];
    size_t n = 0;
    const char *s = name;
    if (*s == '[') {                               /* [v6]:port */
        const char *e = strchr(s, ']');
        if (!e) return 0;
        n = (size_t)(e - s - 1);
        if (n >= sizeof host) return 0;
        memcpy(host, s + 1, n);
    } else {
        /* One colon is host:port; more than one is a bare IPv6 literal. */
        const char *colon = strchr(s, ':');
        n = (colon && !strchr(colon + 1, ':')) ? (size_t)(colon - s) : strlen(s);
        if (n >= sizeof host) return 0;
        memcpy(host, s, n);
    }
    host[n] = '\0';
    if (n == 0) return 0;
    unsigned char buf[16];
    if (inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1) return 0;
    if ((size_t)snprintf(out, cap, "%s", host) >= cap) { out[0] = '\0'; return 0; }
    return 1;
}

int oc_workspace_key(const char *workspace, const char *suffix, char *out, size_t cap) {
    char domain[256];
    if (oc_resolve_domain(workspace, suffix, domain, sizeof domain) != 0) return -1;
    for (char *p = domain; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');

    /* A port counts only if it was typed, and only if it is one. */
    char host[256];
    long port; int bracketed;
    if (ws_split(workspace, host, sizeof host, &port, &bracketed) != 0) return -1;
    if (port < 0) port = 0;
    int n = port ? snprintf(out, cap, "%s:%ld", domain, port) : snprintf(out, cap, "%s", domain);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

#if defined(__EMSCRIPTEN__)
int oc_srv_parse(const unsigned char *answer, int len, char *host, size_t hostcap, int *port) {
    (void)answer; (void)len; (void)host; (void)hostcap; (void)port;
    return -1;
}
#elif !defined(_WIN32)
int oc_srv_parse(const unsigned char *answer, int len, char *host, size_t hostcap, int *port) {
    if (!answer || len <= 0 || !host || hostcap == 0 || !port) return -1;
    ns_msg msg;
    if (ns_initparse(answer, len, &msg) < 0) return -1;
    int n = ns_msg_count(msg, ns_s_an);
    int found = 0, best_prio = 0;
    for (int i = 0; i < n; i++) {
        ns_rr rr;
        if (ns_parserr(&msg, ns_s_an, i, &rr) < 0) continue;
        if (ns_rr_type(rr) != ns_t_srv) continue;
        const unsigned char *rd = ns_rr_rdata(rr);
        if (ns_rr_rdlen(rr) < 7) continue;
        int prio = ns_get16(rd);
        int p    = ns_get16(rd + 4);
        char target[256];
        if (dn_expand(ns_msg_base(msg), ns_msg_end(msg), rd + 6, target, sizeof target) < 0)
            continue;
        if (target[0] == '\0') continue;         /* "." target = service explicitly absent */
        if (!found || prio < best_prio) {
            best_prio = prio; found = 1;
            snprintf(host, hostcap, "%s", target);
            *port = p;
        }
    }
    return found ? 0 : -1;
}
#endif /* !_WIN32 */

/* Query SRV for `_openchime._tcp.<domain>`; 0 + host/port on success, -1 else.
 * POSIX uses res_query + oc_srv_parse; Windows uses DnsQuery, which returns the
 * SRV records already parsed, so there is no raw answer to hand to oc_srv_parse.
 * Both pick the lowest-priority (most-preferred) target. */
static int srv_lookup(const char *domain, char *host, size_t hostcap, int *port) {
    g_srv_attempts++;
    char qname[300];
    if ((size_t)snprintf(qname, sizeof qname, "_openchime._tcp.%s", domain) >= sizeof qname)
        return -1;
#ifdef _WIN32
    /* Explicit ANSI types (PDNS_RECORDA), not the generic DNS_RECORD: under a
     * UNICODE build (the GUI links -municode) the generic maps to the wide
     * variant, but DnsQuery_A fills ANSI records — the fields must be read as
     * ANSI regardless of the UNICODE macro. */
    PDNS_RECORDA recs = NULL;
    if (DnsQuery_A(qname, DNS_TYPE_SRV, DNS_QUERY_STANDARD, NULL,
                   (PDNS_RECORD *)&recs, NULL) != 0)
        return -1;
    int found = 0; unsigned best_prio = 0;
    for (PDNS_RECORDA r = recs; r; r = r->pNext) {
        if (r->wType != DNS_TYPE_SRV) continue;
        const char *tgt = r->Data.SRV.pNameTarget;
        if (!tgt || tgt[0] == '\0' || (tgt[0] == '.' && tgt[1] == '\0')) continue;
        if (!found || r->Data.SRV.wPriority < best_prio) {
            best_prio = r->Data.SRV.wPriority; found = 1;
            snprintf(host, hostcap, "%s", tgt);
            *port = r->Data.SRV.wPort;
        }
    }
    if (recs) DnsFree(recs, DnsFreeRecordList);
    return found ? 0 : -1;
#elif defined(__EMSCRIPTEN__)
    (void)host; (void)hostcap; (void)port;
    return -1;
#else
    unsigned char ans[NS_PACKETSZ];
    int len = res_query(qname, ns_c_in, ns_t_srv, ans, sizeof ans);
    if (len <= 0) return -1;
    return oc_srv_parse(ans, len, host, hostcap, port);
#endif
}

/* Does `domain` resolve to any A/AAAA address? */
static int host_resolves(const char *domain) {
    g_dns_attempts++;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(domain, "443", &hints, &res) != 0) return 0;
    freeaddrinfo(res);
    return 1;
}

oc_resolve_status oc_resolve(const char *workspace, const char *suffix, oc_endpoint *out) {
    if (!out) return OC_RESOLVE_BAD_WORKSPACE;
    oc_sock_startup();   /* idempotent; on Windows getaddrinfo fails until WSAStartup */
    char domain[256];
    if (oc_resolve_domain(workspace, suffix, domain, sizeof domain) != 0)
        return OC_RESOLVE_BAD_WORKSPACE;
    snprintf(out->domain, sizeof out->domain, "%s", domain);

    char typed[256];
    long tport; int bracketed;
    if (ws_split(workspace, typed, sizeof typed, &tport, &bracketed) != 0) return OC_RESOLVE_BAD_WORKSPACE;

    /* An address is used as given, FIRST: no SRV query, no DNS, no `.well-known`
     * request -- nothing at all but the connection that follows. An SRV record
     * or a web document cannot mean anything for an address, and on an
     * air-gapped network a typed address is how the daemon is found. */
    char canon[128]; int v6 = 0;
    if (literal_of(typed, canon, sizeof canon, &v6)) {
        if (tport < 0) return OC_RESOLVE_BAD_WORKSPACE;
        snprintf(out->host, sizeof out->host, "%s", canon);
        out->port = tport > 0 ? (int)tport : 443;   /* OC_DEFAULT_PORT */
        return OC_RESOLVE_OK;
    }

    /* An explicit `:port` pins host:port directly, skipping SRV. */
    int xport = tport > 0 ? (int)tport : 0;
    if (xport > 0) {
        if (!host_resolves(domain)) return OC_RESOLVE_NOT_FOUND;
        snprintf(out->host, sizeof out->host, "%s", domain);
        out->port = xport;
        return OC_RESOLVE_OK;
    }

    /* Preferred: an SRV record names the daemon host + port. */
    if (srv_lookup(domain, out->host, sizeof out->host, &out->port) == 0)
        return OC_RESOLVE_OK;

    /* Fallback: the domain itself, at the standard port -- and the optional
     * `.well-known` document, which is the other half of REQ-010 and the only
     * thing that can move the port off 443 once SRV has said nothing. Asked for
     * only here, because SRV outranks it (ARCH-54) and answering first makes the
     * question moot. */
    if (host_resolves(domain)) {
        snprintf(out->host, sizeof out->host, "%s", domain);
        out->port = 443;                     /* OC_DEFAULT_PORT, as host_resolves uses */
        oc_wellknown wk;
        g_wk_attempts++;
        int wkr = oc_wellknown_fetch(domain, &wk);
        if (wkr == OC_WK_MALFORMED) return OC_RESOLVE_BAD_METADATA;
        if (wkr == OC_WK_OK) {
            if (wk.port) out->port = wk.port;
            snprintf(out->fingerprint, sizeof out->fingerprint, "%s", wk.fingerprint);
        }
        return OC_RESOLVE_OK;
    }
    return OC_RESOLVE_NOT_FOUND;   /* the org name simply doesn't exist (REQ-011) */
}
