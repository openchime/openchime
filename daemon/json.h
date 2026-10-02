/*
 * The daemon's JSON reader for the few documents it is handed whole: ACME's
 * answers, an OIDC provider's discovery document, key set and token response,
 * and an ID token's claims (AUTH.md §8.5). jsmn tokenizes; this walks objects
 * by key and turns strings into C strings -- never truncated: a value that does
 * not fit is refused, because a truncated subject is somebody else's.
 *
 * Off the message path (ARCH-6): a document is parsed once, then read.
 */
#ifndef OPENCHIME_JSON_H
#define OPENCHIME_JSON_H

#include <stddef.h>
#include <stdint.h>

#define JSMN_HEADER
#include "jsmn.h"

typedef struct {
    const char *js;
    jsmntok_t  *t;
    int         n;
} oc_json;

/* Tokenize `len` bytes of `js`, which must outlive the document. 0 or -1. */
int  oc_json_parse(oc_json *d, const char *js, size_t len);
void oc_json_free(oc_json *d);
/* The index just past token `i`'s whole subtree. */
int  oc_json_skip(const oc_json *d, int i);
/* The value of `key` in the object at token `obj`, or -1. */
int  oc_json_get(const oc_json *d, int obj, const char *key);
/* The string at token `i`, unescaped into `out`: 0, or -1 if it is not a
 * string, carries a bad escape or a NUL, or does not fit `cap` whole. */
int  oc_json_str(const oc_json *d, int i, char *out, size_t cap);
/* The string value of `key` in `obj` (oc_json_str's rules); -1 also if absent. */
int  oc_json_get_str(const oc_json *d, int obj, const char *key, char *out, size_t cap);
/* 1 if token `i` is the literal `true`, or the string "true" (some providers'
 * habit for email_verified); else 0. */
int  oc_json_true(const oc_json *d, int i);
/* The non-negative integer at token `i`: 0 and its value, or -1. */
int  oc_json_u64(const oc_json *d, int i, uint64_t *out);

/* JSON-unescape src[0..n) into dst: the byte count, or -1 (oc_json_str). */
long oc_json_unescape(const char *src, size_t n, char *dst, size_t cap);

#endif /* OPENCHIME_JSON_H */
