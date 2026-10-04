/* The local summary model, fetched on first use (sum_fetch.h). */
#define _POSIX_C_SOURCE 200809L
#include "sum_fetch.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "https_client.h"

void oc_sum_model_dir(const char *db_path, char *out, size_t cap) {
    const char *slash = db_path ? strrchr(db_path, '/') : NULL;
    if (!slash) { snprintf(out, cap, "summary"); return; }
    snprintf(out, cap, "%.*s/summary", (int)(slash - db_path), db_path);
}

/* The marker beside the model says which file was checked: its SHA-256 and
 * size. Hashing half a gigabyte at every start is not needed to know it is the
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

int oc_sum_model_ensure(const char *dir, const char *file, const char *url, const char *sha256_hex,
                        uint64_t max_bytes, const volatile int *stop, char *out, size_t cap, char *err,
                        size_t errcap) {
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        snprintf(err, errcap, "cannot make %s", dir);
        return -1;
    }
    char marker[1100], part[1100];
    snprintf(out, cap, "%s/%s", dir, file);
    snprintf(marker, sizeof marker, "%s.ok", out);
    snprintf(part, sizeof part, "%s.part", out);
    if (marker_ok(marker, out, sha256_hex)) return 0;

    fprintf(stderr, "summary: fetching the model from %s\n", url);
    unsigned char sha[32];
    char derr[256] = "";
    if (oc_https_download(url, part, max_bytes, 120000, stop, sha, derr, sizeof derr) != 0) {
        unlink(part);
        snprintf(err, errcap, "fetching the model failed: %s", derr);
        return -1;
    }
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sha[i]);
    if (strcmp(hex, sha256_hex) != 0) {
        unlink(part);
        snprintf(err, errcap, "the model fetched is not the one expected (SHA-256 %s)", hex);
        return -1;
    }
    if (rename(part, out) != 0) {
        unlink(part);
        snprintf(err, errcap, "cannot put the model at %s", out);
        return -1;
    }
    struct stat st;
    FILE *f = fopen(marker, "w");
    if (!f || stat(out, &st) != 0) {
        if (f) fclose(f);
        snprintf(err, errcap, "cannot write %s", marker);
        return -1;
    }
    fprintf(f, "%s %llu\n", sha256_hex, (unsigned long long)st.st_size);
    if (fclose(f) != 0) { snprintf(err, errcap, "cannot write %s", marker); return -1; }
    fprintf(stderr, "summary: model fetched and checked\n");
    return 0;
}
