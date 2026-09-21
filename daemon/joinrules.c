/* OIDC join rules — see joinrules.h. */

#include "joinrules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { RULE_OWNER, RULE_TENANT, RULE_DOMAIN } rule_kind;

typedef struct {
    rule_kind kind;
    char     *idp;     /* RULE_TENANT only */
    char     *value;   /* the address, the organization, or the domain; lowercased */
} rule;

struct oc_join_rules {
    rule  *rules;
    size_t n;
};

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* ASCII case-insensitive equality. Addresses, domains, hosted domains and tenant
 * ids are all compared this way; nothing here is locale-dependent. */
static int eq_nocase(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (; *a && *b; a++, b++)
        if (lower(*a) != lower(*b)) return 0;
    return *a == '\0' && *b == '\0';
}

static char *dup_lower(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (!d) return NULL;
    for (size_t i = 0; i < n; i++) d[i] = lower(s[i]);
    d[n] = '\0';
    return d;
}

static void fail(char *err, size_t cap, const char *why, const char *tok, size_t toklen) {
    if (err && cap) snprintf(err, cap, "%s: \"%.*s\"", why, (int)toklen, tok);
}

/* One address: exactly one '@', something on both sides, no spaces. */
static int plausible_email(const char *s, size_t n) {
    const char *at = memchr(s, '@', n);
    if (!at || at == s || at == s + n - 1) return 0;
    if (memchr(at + 1, '@', (size_t)(s + n - at - 1))) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == ' ' || s[i] == '\t') return 0;
    return 1;
}

static int plausible_domain(const char *s, size_t n) {
    if (n == 0 || !memchr(s, '.', n) || s[0] == '.' || s[n - 1] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = lower(s[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return 0;
    }
    return 1;
}

void oc_join_rules_free(oc_join_rules *r) {
    if (!r) return;
    for (size_t i = 0; i < r->n; i++) { free(r->rules[i].idp); free(r->rules[i].value); }
    free(r->rules);
    free(r);
}

oc_join_rules *oc_join_rules_parse(const char *spec, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    oc_join_rules *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    if (!spec) return r;

    size_t cap = 1;
    for (const char *p = spec; *p; p++) if (*p == ',') cap++;
    r->rules = calloc(cap, sizeof *r->rules);
    if (!r->rules) { free(r); return NULL; }

    const char *p = spec;
    while (*p) {
        const char *end = strchr(p, ',');
        if (!end) end = p + strlen(p);
        const char *s = p, *e = end;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        size_t n = (size_t)(e - s);
        p = *end ? end + 1 : end;
        if (n == 0) continue;   /* a trailing or doubled comma is not a rule */

        rule *ru = &r->rules[r->n];
        if (n > 6 && strncmp(s, "owner:", 6) == 0) {
            if (!plausible_email(s + 6, n - 6)) { fail(err, errcap, "not an address", s, n); goto bad; }
            ru->kind = RULE_OWNER;
            ru->value = dup_lower(s + 6, n - 6);
        } else if (n > 7 && strncmp(s, "domain:", 7) == 0) {
            if (!plausible_domain(s + 7, n - 7)) { fail(err, errcap, "not a domain", s, n); goto bad; }
            ru->kind = RULE_DOMAIN;
            ru->value = dup_lower(s + 7, n - 7);
        } else if (n > 7 && strncmp(s, "tenant:", 7) == 0) {
            const char *idp = s + 7;
            const char *colon = memchr(idp, ':', (size_t)(e - idp));
            if (!colon || colon == idp || colon + 1 == e) {
                fail(err, errcap, "a tenant rule is tenant:<provider>:<organization>", s, n);
                goto bad;
            }
            ru->kind = RULE_TENANT;
            ru->idp = dup_lower(idp, (size_t)(colon - idp));
            ru->value = dup_lower(colon + 1, (size_t)(e - colon - 1));
            if (!ru->idp) goto bad;
        } else {
            fail(err, errcap, "unknown join rule", s, n);
            goto bad;
        }
        if (!ru->value) goto bad;
        r->n++;
    }
    return r;

bad:
    if (err && errcap && !err[0]) snprintf(err, errcap, "out of memory");
    r->n++;   /* the half-built rule's strings are freed with the rest */
    oc_join_rules_free(r);
    return NULL;
}

oc_join_verdict oc_join_rules_eval(const oc_join_rules *r, const char *idp,
                                   const char *tenant, const char *email,
                                   int email_verified) {
    if (!r) return OC_JOIN_DENY;
    const char *addr = (email_verified && email && email[0]) ? email : NULL;
    const char *dom = addr ? strrchr(addr, '@') : NULL;
    if (dom) dom++;

    oc_join_verdict best = OC_JOIN_DENY;
    for (size_t i = 0; i < r->n; i++) {
        const rule *ru = &r->rules[i];
        switch (ru->kind) {
        case RULE_OWNER:
            if (addr && eq_nocase(addr, ru->value)) return OC_JOIN_OWNER;
            break;
        case RULE_DOMAIN:
            if (dom && eq_nocase(dom, ru->value)) best = OC_JOIN_MEMBER;
            break;
        case RULE_TENANT:
            if (idp && idp[0] && tenant && tenant[0] &&
                eq_nocase(idp, ru->idp) && eq_nocase(tenant, ru->value))
                best = OC_JOIN_MEMBER;
            break;
        }
    }
    return best;
}
