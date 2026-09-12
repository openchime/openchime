/* MANUAL smoke test: drives the real oc_blobstore S3 backend (ARCH-70) against
 * a real S3-compatible provider.
 *
 * Deliberately NOT part of `make test`: it needs live credentials and network
 * access, and the committed suite must never require either. `make test` covers
 * this code with an in-process fake S3 (tests/test_blob_s3.c) instead, which is
 * hermetic — but a fake accepts our own SigV4 signatures by construction, so it
 * cannot prove a real provider does. That is the gap this harness closes, and
 * the reason to keep it.
 *
 * Run it after touching blob_s3.c, sigv4.c, or the TLS client:
 *
 *     fly storage create -n <bucket> -o <org> -y      # or any S3 provider
 *     export OPENCHIME_S3_ENDPOINT=fly.storage.tigris.dev
 *     export OPENCHIME_S3_BUCKET=<bucket>
 *     export OPENCHIME_S3_ACCESS_KEY=<key>
 *     export OPENCHIME_S3_SECRET_KEY=<secret>
 *     export OPENCHIME_S3_REGION=auto
 *     make s3-smoke && ./build/s3_smoke
 *     fly storage destroy <bucket> -y                 # clean up
 *
 * Keep credentials out of the repo and out of shell history. Last verified
 * against Tigris: streaming round-trips, sizes, delete semantics, and (by
 * pointing OPENCHIME_S3_ENDPOINT at badssl.com hosts) that expired,
 * self-signed, untrusted-root, and wrong-hostname certificates are rejected.
 *
 * Exercises exactly what the daemon does: streaming put in many chunks,
 * streaming get, size reporting, byte-for-byte round-trip, and delete. */

#include "blobstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, what)                                                      \
    do {                                                                       \
        if (cond) printf("  ok   %s\n", (what));                               \
        else { printf("  FAIL %s\n", (what)); failures++; }                    \
    } while (0)

/* A deterministic, non-repeating pattern so a mis-ordered or truncated
 * stream cannot accidentally compare equal. */
static void fill_pattern(uint8_t *buf, size_t len, uint32_t seed) {
    uint32_t x = seed;
    for (size_t i = 0; i < len; i++) {
        x = x * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(x >> 24);
    }
}

static int round_trip(oc_blobstore *bs, const char *key, size_t size, size_t chunk) {
    printf("\n- blob '%s': %zu bytes in %zu-byte chunks\n", key, size, chunk);
    uint8_t *src = malloc(size), *got = malloc(size);
    if (!src || !got) { printf("  FAIL alloc\n"); failures++; free(src); free(got); return -1; }
    fill_pattern(src, size, (uint32_t)size);

    /* --- streaming write --- */
    oc_blob_writer *w = oc_blob_put_begin(bs, key, size);
    CHECK(w != NULL, "put_begin");
    if (!w) { free(src); free(got); return -1; }
    int wrote_ok = 1;
    for (size_t off = 0; off < size; off += chunk) {
        size_t n = (size - off < chunk) ? size - off : chunk;
        if (oc_blob_put_chunk(w, src + off, n) != 0) { wrote_ok = 0; break; }
    }
    CHECK(wrote_ok, "put_chunk (all chunks accepted)");
    CHECK(oc_blob_put_commit(w) == 0, "put_commit (provider accepted the object)");

    /* --- streaming read --- */
    uint64_t reported = 0;
    oc_blob_reader *r = oc_blob_get_begin(bs, key, &reported);
    CHECK(r != NULL, "get_begin");
    if (!r) { free(src); free(got); return -1; }
    CHECK(reported == (uint64_t)size, "get_begin reports the correct size");

    size_t total = 0;
    for (;;) {
        long n = oc_blob_get_chunk(r, got + total, (size - total) ? (size - total) : 1);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= size) break;
    }
    oc_blob_get_close(r);
    CHECK(total == size, "read back the full byte count");
    CHECK(total == size && memcmp(src, got, size) == 0, "bytes match exactly");

    free(src); free(got);
    return 0;
}

int main(void) {
    printf("S3 smoke test against a real provider\n");
    printf("endpoint: %s  bucket: %s  region: %s\n",
           getenv("OPENCHIME_S3_ENDPOINT"), getenv("OPENCHIME_S3_BUCKET"),
           getenv("OPENCHIME_S3_REGION"));

    /* base_dir is ignored by the S3 backend; credentials in the environment
     * select it (blobstore.c). */
    oc_blobstore *bs = oc_blobstore_open("/tmp/unused-s3-smoke");
    CHECK(bs != NULL, "oc_blobstore_open selected + connected the S3 backend");
    if (!bs) { printf("\nFAILED: could not open the store\n"); return 1; }

    /* Small: single chunk. */
    round_trip(bs, "smoke0001", 11, 64 * 1024);
    /* Multi-chunk: proves the streaming body, not just a one-shot PUT. */
    round_trip(bs, "smoke0002", 300 * 1024, 64 * 1024);
    /* Chunk size that does not divide the total, so the last write is partial. */
    round_trip(bs, "smoke0003", 100000, 7777);

    /* --- delete --- */
    printf("\n- delete\n");
    CHECK(oc_blob_delete(bs, "smoke0001") == 0, "delete returns success");
    oc_blob_reader *gone = oc_blob_get_begin(bs, "smoke0001", NULL);
    CHECK(gone == NULL, "deleted blob is no longer retrievable");
    if (gone) oc_blob_get_close(gone);

    /* Deleting something absent is documented as success, not an error. */
    CHECK(oc_blob_delete(bs, "smoke-never-existed") == 0, "delete of a missing key succeeds");

    /* Clean up the rest so the bucket is empty when we tear it down. */
    oc_blob_delete(bs, "smoke0002");
    oc_blob_delete(bs, "smoke0003");

    oc_blobstore_close(bs);
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL SMOKE CHECKS PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
