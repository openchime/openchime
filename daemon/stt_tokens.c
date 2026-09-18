/* Moonshine's tokenizer, decoding only (stt_moonshine.h). No ONNX Runtime. */
#include "stt_moonshine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *const MOONSHINE_FILE_NAMES[MOONSHINE_FILES] = {
    "frontend.model.ort", "frontend.weights.ort", "encoder.ort", "adapter.ort",
    "cross_kv.ort",       "decoder_kv.ort",       "tokenizer.bin", "streaming_config.json",
};

/* tokenizer.bin is one record per token id, in order: a length byte, or two
 * when the first has its top bit set (len = second * 128 + first - 128), then
 * that many bytes. A zero length is an unused id. */
int moonshine_tokens_load(moonshine_tokens *t, const uint8_t *data, size_t len) {
    memset(t, 0, sizeof *t);
    uint32_t count = 0;
    for (size_t off = 0; off < len; count++) {
        size_t n = data[off++];
        if (n >= 128) {
            if (off >= len) return -1;
            n = (size_t)data[off++] * 128 + n - 128;
        }
        if (n > len - off) return -1;
        off += n;
    }
    if (!count) return -1;
    t->bytes = malloc(count * sizeof *t->bytes);
    t->len = malloc(count * sizeof *t->len);
    if (!t->bytes || !t->len) { moonshine_tokens_free(t); return -1; }
    size_t off = 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t n = data[off++];
        if (n >= 128) n = (size_t)data[off++] * 128 + n - 128;
        if (n > 0xFFFF) { moonshine_tokens_free(t); return -1; }
        t->bytes[i] = data + off;
        t->len[i] = (uint16_t)n;
        off += n;
    }
    t->count = count;
    return 0;
}

void moonshine_tokens_free(moonshine_tokens *t) {
    free(t->bytes);
    free(t->len);
    memset(t, 0, sizeof *t);
}

long moonshine_detok(const moonshine_tokens *t, const int64_t *ids, size_t n, char *out, size_t cap) {
    if (!t->count || !cap) return -1;
    static const char MARK[] = "\xE2\x96\x81"; /* U+2581, the word marker */
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (ids[i] < 0 || (uint64_t)ids[i] >= t->count) continue;
        const uint8_t *b = t->bytes[ids[i]];
        size_t len = t->len[ids[i]];
        if (!len) continue;
        if (len > 2 && b[0] == '<' && b[len - 1] == '>') continue; /* special */
        for (size_t k = 0; k < len;) {
            if (k + 3 <= len && !memcmp(b + k, MARK, 3)) {
                if (w + 1 < cap) out[w++] = ' ';
                k += 3;
            } else {
                if (w + 1 < cap) out[w++] = (char)b[k];
                k++;
            }
        }
    }
    /* Trim, then cut back to a whole UTF-8 character if `cap` split one. */
    size_t s = 0;
    while (s < w && (out[s] == ' ' || out[s] == '\t' || out[s] == '\n')) s++;
    while (w > s && (out[w - 1] == ' ' || out[w - 1] == '\t' || out[w - 1] == '\n')) w--;
    if (s) memmove(out, out + s, w - s);
    w -= s;
    size_t k = w;
    while (k > 0 && ((unsigned char)out[k - 1] & 0xC0) == 0x80) k--;
    if (k > 0 && ((unsigned char)out[k - 1] & 0x80)) {
        unsigned char lead = (unsigned char)out[k - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (w - (k - 1) < need) w = k - 1;
    }
    out[w] = 0;
    return (long)w;
}

size_t moonshine_max_tokens(size_t samples) {
    double seconds = (double)samples / MOONSHINE_RATE;
    size_t n = (size_t)ceil(seconds * MOONSHINE_TOKENS_PER_SECOND);
    return n > MOONSHINE_MAX_TOKENS ? MOONSHINE_MAX_TOKENS : n;
}
