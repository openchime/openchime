/*
 * A minimal CBOR reader (RFC 8949) for what a passkey hands over (AUTH.md
 * §8.6): an attestation object and a COSE key. Unsigned and negative integers,
 * byte and text strings, arrays and maps, of definite length only, nested a
 * bounded depth. Nothing is copied: values point into the input.
 */
#ifndef OPENCHIME_CBOR_H
#define OPENCHIME_CBOR_H

#include <stddef.h>
#include <stdint.h>

#define OC_CBOR_MAX_DEPTH 8

typedef enum { OC_CBOR_UINT = 0, OC_CBOR_NINT = 1, OC_CBOR_BYTES = 2, OC_CBOR_TEXT = 3,
               OC_CBOR_ARRAY = 4, OC_CBOR_MAP = 5, OC_CBOR_TAG = 6, OC_CBOR_SIMPLE = 7 } oc_cbor_major;

typedef struct { const uint8_t *p, *end; } oc_cbor;

/* One item's head: its major type and argument (a length, a count or the
 * integer's value; a negative integer is -1 - `arg`). 0, or -1 at the end of
 * the input, on an indefinite length, or on a reserved form. */
int oc_cbor_head(oc_cbor *c, oc_cbor_major *major, uint64_t *arg);
/* Skip one whole item, nested no deeper than OC_CBOR_MAX_DEPTH. 0 or -1. */
int oc_cbor_skip(oc_cbor *c);
/* In the map at `c` -- positioned on its head -- the value for the text key
 * `key`, or for the integer key `ikey` when `key` is NULL: `out` is set on it.
 * 1 found, 0 not, -1 malformed. */
int oc_cbor_map_get(oc_cbor c, const char *key, int64_t ikey, oc_cbor *out);
/* The byte or text string at `c`: 0 with its bytes, or -1 if it is not one. */
int oc_cbor_string(oc_cbor *c, oc_cbor_major want, const uint8_t **ptr, size_t *len);
/* The integer (either sign) at `c`. 0 or -1. */
int oc_cbor_int(oc_cbor *c, int64_t *v);

#endif /* OPENCHIME_CBOR_H */
