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

int oc_resolve_domain(const char *workspace, const char *suffix, char *out, size_t cap) {
    if (!workspace || !out || cap == 0) return -1;
    const char *s = workspace;
    while (*s == ' ') s++;                       /* skip leading spaces */
    const char *scheme = strstr(s, "://");       /* strip a leading scheme */
    if (scheme) s = scheme + 3;

    char host[256];
    size_t j = 0;
    for (; *s && *s != ':' && *s != '/' && j < sizeof host - 1; s++) host[j++] = *s;
    host[j] = '\0';
    if (j == 0) return -1;

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
         * TOFU pin are stored under: "LocalHost" and "localhost" are the same
         * host, and keeping both would be two entries for one workspace. */
        if ((size_t)snprintf(out, cap, "localhost") >= cap) return -1;
    } else if (!strchr(host, '.') && suffix && *suffix) {   /* bare name -> append suffix */
        if ((size_t)snprintf(out, cap, "%s.%s", host, suffix) >= cap) return -1;
    } else {
        if ((size_t)snprintf(out, cap, "%s", host) >= cap) return -1;
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
    const char *s = workspace;
    while (*s == ' ') s++;
    const char *scheme = strstr(s, "://");
    if (scheme) s = scheme + 3;
    const char *colon = s;
    while (*colon && *colon != ':' && *colon != '/') colon++;
    long port = 0;
    if (*colon == ':') {
        char *end = NULL;
        port = strtol(colon + 1, &end, 10);
        if (end == colon + 1 || port <= 0 || port > 65535) port = 0;
    }
    int n = port ? snprintf(out, cap, "%s:%ld", domain, port) : snprintf(out, cap, "%s", domain);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

#ifndef _WIN32
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
#else
    unsigned char ans[NS_PACKETSZ];
    int len = res_query(qname, ns_c_in, ns_t_srv, ans, sizeof ans);
    if (len <= 0) return -1;
    return oc_srv_parse(ans, len, host, hostcap, port);
#endif
}

/* An explicit `:port` on the workspace (after any scheme), or 0 if none. Lets a
 * user pin a non-standard port, e.g. `chat.acme.com:9000`. */
static int explicit_port(const char *workspace) {
    if (!workspace) return 0;
    const char *s = workspace;
    const char *scheme = strstr(s, "://"); if (scheme) s = scheme + 3;
    const char *slash = strchr(s, '/');
    const char *colon = strchr(s, ':');
    if (!colon || (slash && colon > slash)) return 0;   /* no port, or ':' is in the path */
    long port = 0;
    const char *p = colon + 1;
    if (!*p || *p == '/') return 0;
    for (; *p && *p != '/'; p++) { if (*p < '0' || *p > '9') return 0; port = port * 10 + (*p - '0'); }
    return (port > 0 && port <= 65535) ? (int)port : 0;
}

/* Does `domain` resolve to any A/AAAA address? */
static int host_resolves(const char *domain) {
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

    /* An explicit `:port` pins host:port directly, skipping SRV. */
    int xport = explicit_port(workspace);
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
