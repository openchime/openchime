/*
 * OpenChime client — workspace resolution. See resolve.h.
 */

#include "resolve.h"
#include "sock.h"      /* oc_sock_startup: getaddrinfo needs WSAStartup on Windows */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windns.h>
#else
#  include <resolv.h>
#  include <arpa/nameser.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <sys/socket.h>
#endif

#include <stdio.h>
#include <string.h>

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

    if (!strchr(host, '.') && suffix && *suffix) {   /* bare name -> append suffix */
        if ((size_t)snprintf(out, cap, "%s.%s", host, suffix) >= cap) return -1;
    } else {
        if ((size_t)snprintf(out, cap, "%s", host) >= cap) return -1;
    }
    return 0;
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

    /* Fallback: the domain itself, at the standard port. */
    if (host_resolves(domain)) {
        snprintf(out->host, sizeof out->host, "%s", domain);
        out->port = 443;
        return OC_RESOLVE_OK;
    }
    return OC_RESOLVE_NOT_FOUND;   /* the org name simply doesn't exist (REQ-011) */
}
