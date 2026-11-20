/* voice.bin: mapping the voice model's data and finding its tensors (ttskit.h
 * has the format). The file is used where it lies; opening it reads the header
 * and the index and checks every entry against the file, so a tensor handed
 * out later is always whole, aligned and of the size its shape says. */
#define _POSIX_C_SOURCE 200809L
#include "tts_priv.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rd64(const uint8_t *p) { return (uint64_t)tts_rd32(p) | (uint64_t)tts_rd32(p + 4) << 32; }

static size_t dtype_size(uint32_t t) { return t == VOICE_F32 ? 4 : t == VOICE_U8 || t == VOICE_I8 ? 1 : 0; }

static uint64_t round64(uint64_t n) { return (n + (VOICE_ALIGN - 1)) & ~(uint64_t)(VOICE_ALIGN - 1); }

/* Entry i against the file: why it is wrong, or NULL. `prev_end` is where the
 * previous entry's data ended (the data offset for the first). */
static const char *bad_entry(const uint8_t *e, const uint8_t *prev, size_t n, uint64_t *prev_end) {
    const char *name = (const char *)e;
    if (!memchr(e, '\0', VOICE_NAME_MAX) || !name[0]) return "a name that is empty or unterminated";
    if (prev && strcmp((const char *)prev, name) >= 0) return "a name out of order or repeated";
    uint32_t dtype = tts_rd32(e + 48), ndim = tts_rd32(e + 52);
    size_t es = dtype_size(dtype);
    if (!es) return "an unknown element type";
    if (ndim < 1 || ndim > VOICE_MAX_DIMS) return "a rank outside 1..4";
    uint64_t count = 1;
    for (uint32_t i = 0; i < VOICE_MAX_DIMS; i++) {
        uint32_t d = tts_rd32(e + 56 + 4 * i);
        if (i >= ndim) {
            if (d) return "a dimension past its rank";
            continue;
        }
        if (!d || count > UINT64_MAX / 4 / d) return "an empty or impossible dimension";
        count *= d;
    }
    uint64_t off = rd64(e + 72), bytes = rd64(e + 80);
    if (bytes != count * es) return "a size that disagrees with its shape";
    if (off % VOICE_ALIGN) return "data that is not 64-byte aligned";
    if (off < *prev_end) return "data that overlaps the index or another tensor";
    if (off > n || bytes > n - off) return "data past the end of the file";
    *prev_end = off + bytes;
    return NULL;
}

int voice_map_open(const char *path, const tts_map *mem, voice *v, char *err, size_t errcap) {
    memset(v, 0, sizeof *v);
    if (mem ? (v->map = *mem, v->map.owned = 0, !mem->p) : tts_map_open(path, &v->map) != 0) { tts_seterr(err, errcap, "cannot map %s", path); return TTS_OPEN_ABSENT; }
    const uint8_t *p = v->map.p;
    size_t n = v->map.n;
    if (n < VOICE_HEADER || memcmp(p, VOICE_MAGIC, 8) != 0 || tts_rd32(p + 8) != VOICE_VERSION) {
        tts_seterr(err, errcap, "%s is not a version %u voice file", path, VOICE_VERSION);
        voice_map_close(v);
        return -1;
    }
    /* The kernels read whole vectors from the data: lent bytes must be aligned
     * as a mapping is. */
    if ((uintptr_t)p % VOICE_ALIGN) {
        tts_seterr(err, errcap, "%s is not 64-byte aligned in memory", path);
        voice_map_close(v);
        return -1;
    }
    uint32_t count = tts_rd32(p + 12);
    uint64_t data = rd64(p + 24), size = rd64(p + 32);
    if (tts_rd32(p + 16) != VOICE_HEADER || tts_rd32(p + 20) != VOICE_ENTRY || count == 0 ||
        (n - VOICE_HEADER) / VOICE_ENTRY < count || data != round64(VOICE_HEADER + (uint64_t)count * VOICE_ENTRY) ||
        size != n || data > n) {
        tts_seterr(err, errcap, "%s is truncated or corrupt", path);
        voice_map_close(v);
        return -1;
    }
    uint64_t end = data;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + VOICE_HEADER + (size_t)i * VOICE_ENTRY;
        const char *why = bad_entry(e, i ? e - VOICE_ENTRY : NULL, n, &end);
        if (why) {
            tts_seterr(err, errcap, "%s is corrupt: tensor %u has %s", path, i, why);
            voice_map_close(v);
            return -1;
        }
    }
    if (end != n) {
        tts_seterr(err, errcap, "%s is truncated or corrupt", path);
        voice_map_close(v);
        return -1;
    }
    v->count = count;
    v->index = p + VOICE_HEADER;
    return 0;
}

void voice_map_close(voice *v) {
    tts_map_close(&v->map);
    memset(v, 0, sizeof *v);
}

static voice *open_any(const char *path, const tts_map *mem, char *err, size_t errcap) {
    voice *v = calloc(1, sizeof *v);
    if (!v) { tts_seterr(err, errcap, "out of memory"); return NULL; }
    if (voice_map_open(path, mem, v, err, errcap) != 0) { free(v); return NULL; }
    return v;
}

voice *voice_open(const char *path, char *err, size_t errcap) { return open_any(path, NULL, err, errcap); }

voice *voice_open_mem(const void *bytes, size_t len, char *err, size_t errcap) {
    tts_map m = { bytes, len, 0 };
    return open_any("voice data", &m, err, errcap);
}

void voice_close(voice *v) {
    if (!v) return;
    voice_map_close(v);
    free(v);
}

size_t voice_tensor_count(const voice *v) { return v ? v->count : 0; }

int voice_tensor_at(const voice *v, size_t i, voice_tensor *t) {
    if (!v || i >= v->count) return 0;
    const uint8_t *e = v->index + i * VOICE_ENTRY;
    memset(t, 0, sizeof *t);
    t->name = (const char *)e;
    t->dtype = (int)tts_rd32(e + 48);
    t->ndim = (int)tts_rd32(e + 52);
    for (int d = 0; d < VOICE_MAX_DIMS; d++) t->dims[d] = (int)tts_rd32(e + 56 + 4 * d);
    t->data = v->map.p + rd64(e + 72);
    t->bytes = (size_t)rd64(e + 80);
    uint32_t s = tts_rd32(e + 88);
    memcpy(&t->scale, &s, sizeof s);
    t->zero_point = (int32_t)tts_rd32(e + 92);
    return 1;
}

int voice_tensor_get(const voice *v, const char *name, voice_tensor *t) {
    if (!v || !name) return 0;
    size_t lo = 0, hi = v->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(name, (const char *)(v->index + mid * VOICE_ENTRY));
        if (c == 0) return voice_tensor_at(v, mid, t);
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}
