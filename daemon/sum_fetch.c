/* The local summary model, made on first use (sum_fetch.h). */
#define _POSIX_C_SOURCE 200809L
#include "sum_fetch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/sha256.h>

#include "https_client.h"
#include "summary_pack.h"

static const oc_sum_source_file SOURCE_FILES[] = {
    { "config.json", "config.json", OC_SUM_SOURCE_CONFIG_SHA256, 1u << 20 },
    { "tokenizer.json", "tokenizer.json", OC_SUM_SOURCE_TOKENIZER_SHA256, 64u << 20 },
    { "LICENSE", "LICENSE", OC_SUM_SOURCE_LICENSE_SHA256, 1u << 20 },
    { "model.safetensors-00001-of-00001.safetensors", "model.safetensors", OC_SUM_SOURCE_WEIGHTS_SHA256, 4548221488ull },
};

const oc_sum_source OC_SUM_SOURCE = {
    OC_SUM_SOURCE_URL, SOURCE_FILES, (int)(sizeof SOURCE_FILES / sizeof *SOURCE_FILES),
    OC_SUM_PACK_SHA256,
};

void oc_sum_model_dir(const char *db_path, char *out, size_t cap) {
    const char *slash = db_path ? strrchr(db_path, '/') : NULL;
    if (!slash) { snprintf(out, cap, "summary"); return; }
    snprintf(out, cap, "%.*s/summary", (int)(slash - db_path), db_path);
}

static void hex_of(const unsigned char sha[32], char hex[65]) {
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sha[i]);
}

static int sha_file(const char *path, unsigned char out[32]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    static unsigned char buf[1 << 20];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) mbedtls_sha256_update(&c, buf, (size_t)r);
    close(fd);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
    return r < 0 ? -1 : 0;
}

/* The marker beside the model says which file was checked: its SHA-256 and
 * size. Hashing 1.3 GB at every start is not needed to know it is the
 * file that was checked; the size and the marker together are. */
static int marker_ok(const char *marker, const char *path, const char *sha) {
    FILE *f = fopen(marker, "r");
    if (!f) return 0;
    char line[160] = "";
    int ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok) return 0;
    unsigned long long size = 0;
    char hex[80] = "";
    if (sscanf(line, "%79s %llu", hex, &size) != 2 || strcmp(hex, sha) != 0) return 0;
    struct stat st;
    return stat(path, &st) == 0 && (unsigned long long)st.st_size == size;
}

static void remove_sources(const char *srcdir, const oc_sum_source *src) {
    char p[1200];
    for (int i = 0; i < src->n_files; i++) {
        snprintf(p, sizeof p, "%s/%s", srcdir, src->files[i].save_as);
        unlink(p);
        snprintf(p, sizeof p, "%s/%s.part", srcdir, src->files[i].save_as);
        unlink(p);
    }
    rmdir(srcdir);
}

/* One source file into srcdir, checked; kept when already there and right.
 * Its SHA-256 into sha. */
static int fetch_one(const char *srcdir, const oc_sum_source *src, const oc_sum_source_file *sf,
                     const volatile int *stop, unsigned char sha[32], char *err, size_t errcap) {
    char path[1200], part[1300], url[1200], hex[65];
    snprintf(path, sizeof path, "%s/%s", srcdir, sf->save_as);
    snprintf(part, sizeof part, "%s.part", path);
    if (sha_file(path, sha) == 0) {
        hex_of(sha, hex);
        if (!strcmp(hex, sf->sha256)) return 0;
        unlink(path);
    }
    snprintf(url, sizeof url, "%s%s", src->base_url, sf->name);
    fprintf(stderr, "summary: fetching %s\n", url);
    char derr[256] = "";
    if (oc_https_download(url, part, sf->max_bytes, 120000, stop, sha, derr, sizeof derr) != 0) {
        unlink(part);
        snprintf(err, errcap, "fetching %s failed: %s", sf->name, derr);
        return -1;
    }
    hex_of(sha, hex);
    if (strcmp(hex, sf->sha256) != 0) {
        unlink(part);
        snprintf(err, errcap, "%s is not the one expected (SHA-256 %s)", sf->name, hex);
        return -1;
    }
    if (rename(part, path) != 0) {
        unlink(part);
        snprintf(err, errcap, "cannot put %s at %s", sf->name, path);
        return -1;
    }
    return 0;
}

int oc_sum_model_ensure(const char *dir, const oc_sum_source *src, const volatile int *stop,
                        int (*pause)(void *ctx), void *pause_ctx, char *out, size_t cap, char *err, size_t errcap) {
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        snprintf(err, errcap, "cannot make %s", dir);
        return -1;
    }
    char marker[1100], srcdir[1100], fresh[1200], lic[1100], p[1200];
    snprintf(out, cap, "%s/%s", dir, OC_SUM_MODEL_FILE);
    snprintf(marker, sizeof marker, "%s.ok", out);
    if (marker_ok(marker, out, src->pack_sha256)) return 0;
    unlink(marker);
    unlink(out);
    snprintf(srcdir, sizeof srcdir, "%s/source", dir);
    if (mkdir(srcdir, 0755) != 0 && errno != EEXIST) {
        snprintf(err, errcap, "cannot make %s", srcdir);
        return -1;
    }

    /* The sources, each checked; their hashes for summary.bin's header. */
    uint8_t sha[SP_FILES][32];
    for (int i = 0; i < src->n_files; i++) {
        unsigned char s[32];
        if (stop && *stop) { snprintf(err, errcap, "stopped"); goto fail; }
        if (fetch_one(srcdir, src, &src->files[i], stop, s, err, errcap) != 0) goto fail;
        for (int k = 0; k < SP_FILES; k++)
            if (!strcmp(src->files[i].save_as, OC_SUMMARY_INPUTS[k])) memcpy(sha[k], s, 32);
    }
    fprintf(stderr, "summary: model fetched and checked; converting it\n");

    /* The conversion, then its result checked. */
    snprintf(fresh, sizeof fresh, "%s.new", out);
    char perr[300] = "";
    if (oc_summary_pack(srcdir, fresh, 0, (const uint8_t(*)[32])sha, pause, pause_ctx, NULL, perr, sizeof perr) != 0) {
        snprintf(err, errcap, "converting the model failed: %s", perr);
        unlink(fresh);
        goto fail;
    }
    unsigned char rs[32];
    char hex[65];
    if (sha_file(fresh, rs) != 0) { snprintf(err, errcap, "cannot read %s", fresh); unlink(fresh); goto fail; }
    hex_of(rs, hex);
    if (strcmp(hex, src->pack_sha256) != 0) {
        snprintf(err, errcap, "the converted model is not the one expected (SHA-256 %s)", hex);
        unlink(fresh);
        goto fail;
    }
    if (rename(fresh, out) != 0) { snprintf(err, errcap, "cannot put the model at %s", out); unlink(fresh); goto fail; }
    struct stat st;
    FILE *f = fopen(marker, "w");
    if (!f || stat(out, &st) != 0) {
        if (f) fclose(f);
        snprintf(err, errcap, "cannot write %s", marker);
        goto fail;
    }
    fprintf(f, "%s %llu\n", src->pack_sha256, (unsigned long long)st.st_size);
    if (fclose(f) != 0) { snprintf(err, errcap, "cannot write %s", marker); goto fail; }
    /* The licence travels with the model; the sources go. */
    snprintf(p, sizeof p, "%s/LICENSE", srcdir);
    snprintf(lic, sizeof lic, "%s/LICENSE", dir);
    rename(p, lic);
    remove_sources(srcdir, src);
    fprintf(stderr, "summary: model converted and checked\n");
    return 0;
fail:
    unlink(marker);
    unlink(out);
    remove_sources(srcdir, src);
    return -1;
}
