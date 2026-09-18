/* Where read-aloud's data lives, and whether it matches (tts_data.h). */
#include "tts_data.h"

#include <mbedtls/sha256.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC "openchime-tts-data 1"

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static int is_dir(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int oc_data_dir_find(const char *var, const char *env, const char *system_dir, const char *exe_dir,
                     const char *beside, char *out, size_t cap, char *err, size_t errcap) {
    if (env && *env) {
        if (!is_dir(env)) { seterr(err, errcap, "%s=%s is not a directory", var, env); return 0; }
        return snprintf(out, cap, "%s", env) < (int)cap;
    }
    if (is_dir(system_dir)) return snprintf(out, cap, "%s", system_dir) < (int)cap;
    if (exe_dir && *exe_dir) {
        char path[1024];
        if (snprintf(path, sizeof path, "%s/%s", exe_dir, beside) < (int)sizeof path && is_dir(path))
            return snprintf(out, cap, "%s", path) < (int)cap;
    }
    seterr(err, errcap, "no data: neither %s nor %s/%s exists",
           system_dir ? system_dir : "(none)", exe_dir && *exe_dir ? exe_dir : "(executable)", beside);
    return 0;
}

int oc_tts_data_dir(const char *env, const char *system_dir, const char *exe_dir,
                    char *out, size_t cap, char *err, size_t errcap) {
    return oc_data_dir_find("OPENCHIME_TTS_DATA_DIR", env, system_dir, exe_dir, "voices",
                            out, cap, err, errcap);
}

int oc_tts_exe_dir(char *out, size_t cap) {
    char path[1024];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash) return 0;
    *slash = '\0';
    return snprintf(out, cap, "%s", path[0] ? path : "/") < (int)cap;
}

/* A relative path inside the data directory, and nothing that could leave it. */
static int safe_name(const char *name) {
    return name && *name && name[0] != '/' && !strstr(name, "..");
}

static int sha256_file(const char *path, char hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    unsigned char buf[1 << 16], dig[32];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) mbedtls_sha256_update(&c, buf, n);
    int bad = ferror(f);
    fclose(f);
    mbedtls_sha256_finish(&c, dig);
    mbedtls_sha256_free(&c);
    if (bad) return -1;
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", dig[i]);
    return 0;
}

int oc_tts_data_write_manifest(const char *dir, const char *version, const char *const *files,
                               char *err, size_t errcap) {
    char path[1024], tmp[1040];
    snprintf(path, sizeof path, "%s/%s", dir, OC_TTS_DATA_MANIFEST);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *out = fopen(tmp, "w");
    if (!out) { seterr(err, errcap, "cannot write %s", tmp); return -1; }
    fprintf(out, "%s\nversion %s\n", MAGIC, version);
    for (size_t i = 0; files[i]; i++) {
        char fp[1024], hex[65];
        snprintf(fp, sizeof fp, "%s/%s", dir, files[i]);
        if (!safe_name(files[i]) || sha256_file(fp, hex) != 0) {
            fclose(out); unlink(tmp);
            seterr(err, errcap, "cannot hash %s", fp);
            return -1;
        }
        fprintf(out, "sha256 %s %s\n", hex, files[i]);
    }
    int bad = ferror(out);
    if (fclose(out) != 0 || bad || rename(tmp, path) != 0) {
        unlink(tmp);
        seterr(err, errcap, "cannot write %s", path);
        return -1;
    }
    return 0;
}

int oc_tts_data_verify(const char *dir, const char *version, const char *const *required,
                       char *err, size_t errcap) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, OC_TTS_DATA_MANIFEST);
    FILE *f = fopen(path, "r");
    if (!f) { seterr(err, errcap, "%s: no manifest", dir); return -1; }

    char line[2048];
    if (!fgets(line, sizeof line, f) || strncmp(line, MAGIC, strlen(MAGIC)) != 0 ||
        (line[strlen(MAGIC)] != '\n' && line[strlen(MAGIC)] != '\0')) {
        fclose(f); seterr(err, errcap, "%s is not a voice data manifest", path); return -1;
    }
    if (!fgets(line, sizeof line, f) || strncmp(line, "version ", 8) != 0) {
        fclose(f); seterr(err, errcap, "%s names no version", path); return -1;
    }
    line[strcspn(line, "\n")] = '\0';
    if (strcmp(line + 8, version) != 0) {
        fclose(f);
        seterr(err, errcap, "%s is for %s, not %s", dir, line + 8, version);
        return -1;
    }

    /* Every listed file must hash as stated; every required one must be listed. */
    size_t nreq = 0;
    while (required && required[nreq]) nreq++;
    int seen[64] = { 0 };
    if (nreq > sizeof seen / sizeof seen[0]) { fclose(f); seterr(err, errcap, "too many required files"); return -1; }

    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (!line[0]) continue;
        char want[65], name[1024];
        if (sscanf(line, "sha256 %64s %1023s", want, name) != 2 || strlen(want) != 64 || !safe_name(name)) {
            fclose(f); seterr(err, errcap, "%s: malformed line '%s'", path, line); return -1;
        }
        char fp[2048], got[65];
        snprintf(fp, sizeof fp, "%s/%s", dir, name);
        if (sha256_file(fp, got) != 0) { fclose(f); seterr(err, errcap, "%s: missing", fp); return -1; }
        if (strcmp(got, want) != 0) { fclose(f); seterr(err, errcap, "%s: does not match the manifest", fp); return -1; }
        for (size_t i = 0; i < nreq; i++)
            if (strcmp(required[i], name) == 0) seen[i] = 1;
    }
    fclose(f);
    for (size_t i = 0; i < nreq; i++)
        if (!seen[i]) { seterr(err, errcap, "%s: manifest does not list %s", dir, required[i]); return -1; }
    return 0;
}

int oc_tts_map_open(const char *path, oc_tts_map *m) {
    m->p = NULL;
    m->n = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return -1; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;
    m->p = p;
    m->n = (size_t)st.st_size;
    return 0;
}

void oc_tts_map_close(oc_tts_map *m) {
    if (m->p) munmap((void *)m->p, m->n);
    m->p = NULL;
    m->n = 0;
}
