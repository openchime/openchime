/* The daemon's JSON reader -- see json.h. */

#include "json.h"

#include <stdlib.h>
#include <string.h>

int oc_json_parse(oc_json *d, const char *js, size_t len) {
    jsmn_parser p;
    int cap = 256;
    d->js = js; d->t = NULL; d->n = 0;
    for (;;) {
        jsmntok_t *t = realloc(d->t, (size_t)cap * sizeof *t);
        if (!t) { oc_json_free(d); return -1; }
        d->t = t;
        jsmn_init(&p);
        int n = jsmn_parse(&p, js, len, d->t, (unsigned)cap);
        if (n == JSMN_ERROR_NOMEM && cap < 16384) { cap *= 4; continue; }
        if (n <= 0) { oc_json_free(d); return -1; }
        d->n = n;
        return 0;
    }
}

void oc_json_free(oc_json *d) { free(d->t); d->t = NULL; d->n = 0; }

int oc_json_skip(const oc_json *d, int i) {
    int end = i + 1;
    if (d->t[i].type == JSMN_OBJECT || d->t[i].type == JSMN_ARRAY) {
        int kids = d->t[i].size * (d->t[i].type == JSMN_OBJECT ? 2 : 1);
        for (int k = 0; k < kids && end < d->n; k++) end = oc_json_skip(d, end);
    }
    return end;
}

int oc_json_get(const oc_json *d, int obj, const char *key) {
    if (obj < 0 || obj >= d->n || d->t[obj].type != JSMN_OBJECT) return -1;
    size_t kl = strlen(key);
    int i = obj + 1;
    for (int k = 0; k < d->t[obj].size && i + 1 < d->n; k++) {
        const jsmntok_t *kt = &d->t[i];
        if (kt->type == JSMN_STRING && (size_t)(kt->end - kt->start) == kl &&
            !memcmp(d->js + kt->start, key, kl)) return i + 1;
        i = oc_json_skip(d, i + 1);
    }
    return -1;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex4(const char *p, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_val(p[i]);
        if (h < 0) return 0;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return 1;
}

long oc_json_unescape(const char *src, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp;
        char c = src[i];
        if (c != '\\') {
            cp = (unsigned char)c;
            if (cp == 0) return -1;
            if (o + 1 >= cap) return -1;
            dst[o++] = c;
            continue;
        }
        if (++i >= n) return -1;
        switch (src[i]) {
        case '"': cp = '"'; break;   case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;   case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;  case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;  case 't': cp = '\t'; break;
        case 'u':
            if (i + 4 >= n) return -1;
            if (!hex4(src + i + 1, &cp)) return -1;
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {          /* surrogate pair */
                uint32_t lo;
                if (i + 6 >= n || src[i + 1] != '\\' || src[i + 2] != 'u') return -1;
                if (!hex4(src + i + 3, &lo) || lo < 0xDC00 || lo > 0xDFFF) return -1;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                i += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return -1;
            }
            break;
        default: return -1;
        }
        if (cp == 0) return -1;
        uint8_t u[4]; size_t ul;
        if (cp < 0x80)        { u[0] = (uint8_t)cp; ul = 1; }
        else if (cp < 0x800)  { u[0] = (uint8_t)(0xC0 | (cp >> 6));
                                u[1] = (uint8_t)(0x80 | (cp & 0x3F)); ul = 2; }
        else if (cp < 0x10000){ u[0] = (uint8_t)(0xE0 | (cp >> 12));
                                u[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                                u[2] = (uint8_t)(0x80 | (cp & 0x3F)); ul = 3; }
        else                  { u[0] = (uint8_t)(0xF0 | (cp >> 18));
                                u[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
                                u[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                                u[3] = (uint8_t)(0x80 | (cp & 0x3F)); ul = 4; }
        if (o + ul >= cap) return -1;
        memcpy(dst + o, u, ul);
        o += ul;
    }
    dst[o] = '\0';
    return (long)o;
}

int oc_json_str(const oc_json *d, int i, char *out, size_t cap) {
    if (!cap) return -1;
    out[0] = '\0';
    if (i < 0 || i >= d->n || d->t[i].type != JSMN_STRING) return -1;
    if (oc_json_unescape(d->js + d->t[i].start, (size_t)(d->t[i].end - d->t[i].start), out, cap) < 0) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

int oc_json_get_str(const oc_json *d, int obj, const char *key, char *out, size_t cap) {
    return oc_json_str(d, oc_json_get(d, obj, key), out, cap);
}

int oc_json_true(const oc_json *d, int i) {
    if (i < 0 || i >= d->n) return 0;
    const jsmntok_t *t = &d->t[i];
    size_t l = (size_t)(t->end - t->start);
    return (t->type == JSMN_PRIMITIVE || t->type == JSMN_STRING) && l == 4 && !memcmp(d->js + t->start, "true", 4);
}

int oc_json_u64(const oc_json *d, int i, uint64_t *out) {
    if (i < 0 || i >= d->n || d->t[i].type != JSMN_PRIMITIVE) return -1;
    uint64_t v = 0;
    int digits = 0;
    for (int p = d->t[i].start; p < d->t[i].end; p++) {
        char c = d->js[p];
        if (c < '0' || c > '9' || v > (UINT64_MAX - 9) / 10) return -1;
        v = v * 10 + (uint64_t)(c - '0');
        digits++;
    }
    if (!digits) return -1;
    *out = v;
    return 0;
}
