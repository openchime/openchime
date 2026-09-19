/* Screen sharing's packet logic — see oc_share.h and docs/VIDEO.md §4-§6. */
#include "oc_share.h"

#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }
static uint32_t get16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t get32(const uint8_t *p) { return (get16(p) << 16) | get16(p + 2); }
static uint64_t get64(const uint8_t *p) { return ((uint64_t)get32(p) << 32) | get32(p + 4); }

/* ---- control packets ------------------------------------------------------------ */

static size_t ctl_head(uint8_t *out, uint64_t target, int kind) {
    out[0] = OC_CALLPKT_CONTROL;
    put64(out + 1, target);
    out[9] = (uint8_t)kind;
    return 10;
}

size_t oc_share_ctl_pli(uint8_t *out, uint64_t target) { return ctl_head(out, target, OC_SHARE_PLI); }

size_t oc_share_ctl_report(uint8_t *out, uint64_t target, int loss_permille, uint32_t kbps) {
    size_t n = ctl_head(out, target, OC_SHARE_REPORT);
    if (loss_permille < 0) loss_permille = 0;
    if (loss_permille > 1000) loss_permille = 1000;
    put16(out + n, (uint32_t)loss_permille);
    put32(out + n + 2, kbps);
    return n + 6;
}

size_t oc_share_ctl_nack(uint8_t *out, uint64_t target, uint32_t frame, const uint16_t *frags, int n) {
    size_t len = ctl_head(out, target, OC_SHARE_NACK);
    if (n < 0) n = 0;
    if (n > 64) n = 64;
    put32(out + len, frame);
    put16(out + len + 4, (uint32_t)n);
    len += 6;
    for (int i = 0; i < n; i++, len += 2) put16(out + len, frags[i]);
    return len;
}

/* ---- the sharer ------------------------------------------------------------------ */

int oc_share_tx_init(oc_share_tx *t, uint64_t self) {
    memset(t, 0, sizeof *t);
    t->self = self;
    t->hist = calloc(OC_SHARE_HISTORY, sizeof *t->hist);
    t->kbps = OC_SHARE_KBPS_START;
    return t->hist ? 0 : -1;
}

void oc_share_tx_free(oc_share_tx *t) {
    free(t->hist);
    t->hist = NULL;
}

void oc_share_tx_restart(oc_share_tx *t, int64_t now_ms) {
    for (int i = 0; i < OC_SHARE_HISTORY; i++) t->hist[i].len = 0;
    t->kbps = OC_SHARE_KBPS_START;
    t->rate_at_ms = now_ms;
    t->cut_at_ms = 0;
    t->key_at_ms = 0;
    t->have_key = 0;
    t->key_wanted = 1;
    memset(t->viewers, 0, sizeof t->viewers);
}

int oc_share_tx_frame(oc_share_tx *t, const uint8_t *data, size_t len, int keyframe,
                      int width, int height, int64_t now_ms, oc_share_emit emit, void *ctx) {
    size_t nfrags = (len + OC_SHARE_FRAG - 1) / OC_SHARE_FRAG;
    if (len == 0 || nfrags > OC_SHARE_MAX_FRAGS || width <= 0 || height <= 0 ||
        width > OC_SHARE_MAX_DIM || height > OC_SHARE_MAX_DIM) {
        t->key_wanted = 1;          /* what follows cannot be decoded without it */
        return -1;
    }
    uint32_t frame = t->next_frame++;
    for (size_t i = 0; i < nfrags; i++) {
        size_t off = i * OC_SHARE_FRAG, n = len - off < OC_SHARE_FRAG ? len - off : OC_SHARE_FRAG;
        oc_share_sent *s = &t->hist[t->hist_at];
        t->hist_at = (t->hist_at + 1) % OC_SHARE_HISTORY;
        uint8_t *p = s->pt;
        p[0] = OC_CALLPKT_VIDEO;
        put32(p + 1, frame);
        put16(p + 5, (uint32_t)i);
        put16(p + 7, (uint32_t)nfrags);
        p[9] = keyframe ? 1 : 0;
        put16(p + 10, (uint32_t)width);
        put16(p + 12, (uint32_t)height);
        memcpy(p + OC_SHARE_VIDEO_HDR, data + off, n);
        s->frame = frame;
        s->frag = (uint16_t)i;
        s->len = (uint16_t)(OC_SHARE_VIDEO_HDR + n);
        s->at_ms = now_ms;
        emit(ctx, p, s->len);
        t->frags++;
    }
    t->frames++;
    t->bytes += len;
    if (keyframe) { t->keyframes++; t->key_at_ms = now_ms; t->have_key = 1; t->key_wanted = 0; }
    return 0;
}

static void resend(oc_share_tx *t, uint32_t frame, int frag, int64_t now_ms, oc_share_emit emit, void *ctx) {
    for (int i = 0; i < OC_SHARE_HISTORY; i++) {
        oc_share_sent *s = &t->hist[i];
        if (!s->len || s->frame != frame || (frag >= 0 && s->frag != frag)) continue;
        if (now_ms - s->at_ms > OC_SHARE_HISTORY_MS) continue;
        emit(ctx, s->pt, s->len);
        t->resent++;
        if (frag >= 0) return;
    }
}

void oc_share_tx_control(oc_share_tx *t, uint64_t from, const uint8_t *body, size_t len,
                         int64_t now_ms, oc_share_emit emit, void *ctx) {
    if (len < 9 || get64(body) != t->self) return;
    int kind = body[8];
    body += 9; len -= 9;
    if (kind == OC_SHARE_NACK) {
        if (len < 6) return;
        uint32_t frame = get32(body);
        uint32_t n = get16(body + 4);
        if (n > 64 || len < 6 + 2 * (size_t)n) return;
        t->nacks++;
        if (n == 0) { resend(t, frame, -1, now_ms, emit, ctx); return; }
        for (uint32_t i = 0; i < n; i++) resend(t, frame, (int)get16(body + 6 + 2 * i), now_ms, emit, ctx);
    } else if (kind == OC_SHARE_PLI) {
        t->plis++;
        t->key_wanted = 1;
    } else if (kind == OC_SHARE_REPORT) {
        if (len < 6) return;
        int loss = (int)get16(body);
        t->reports++;
        oc_share_viewer *v = NULL;
        for (int i = 0; i < 32 && !v; i++) if (t->viewers[i].user == from) v = &t->viewers[i];
        for (int i = 0; i < 32 && !v; i++) if (!t->viewers[i].user) v = &t->viewers[i];
        if (!v) {
            v = &t->viewers[0];
            for (int i = 1; i < 32; i++) if (t->viewers[i].at_ms < v->at_ms) v = &t->viewers[i];
        }
        v->user = from;
        v->loss_permille = loss;
        v->at_ms = now_ms;
        /* The rate is what the worst-placed viewer can take: any of them losing
         * more than 5% halves it (once a second at most), and growth waits while
         * anyone loses more than 2%. */
        if (loss > 50 && now_ms - t->cut_at_ms >= 1000) {
            t->kbps /= 2;
            if (t->kbps < OC_SHARE_KBPS_MIN) t->kbps = OC_SHARE_KBPS_MIN;
            t->cut_at_ms = t->rate_at_ms = now_ms;
        } else if (loss > 20) {
            t->rate_at_ms = now_ms;
        }
    }
}

int oc_share_tx_want_keyframe(oc_share_tx *t, int64_t now_ms) {
    if (!t->have_key) return 1;
    if (t->key_wanted && now_ms - t->key_at_ms >= OC_SHARE_KEY_MIN_MS) return 1;
    return now_ms - t->key_at_ms >= OC_SHARE_KEY_MAX_MS;
}

int oc_share_tx_kbps(oc_share_tx *t, int64_t now_ms) {
    if (now_ms - t->rate_at_ms >= 1000) {
        t->kbps += t->kbps / 10 + 1;
        if (t->kbps > OC_SHARE_KBPS_MAX) t->kbps = OC_SHARE_KBPS_MAX;
        t->rate_at_ms = now_ms;
    }
    return t->kbps;
}

void oc_share_tx_forget(oc_share_tx *t, uint64_t user) {
    for (int i = 0; i < 32; i++) if (t->viewers[i].user == user) memset(&t->viewers[i], 0, sizeof t->viewers[i]);
}

/* ---- a viewer ---------------------------------------------------------------------- */

void oc_share_rx_init(oc_share_rx *r, uint64_t sharer) {
    memset(r, 0, sizeof *r);
    r->sharer = sharer;
    r->handed = -1;
    r->born_ms = -1;
}

static void slot_drop(oc_share_slot *s) {
    free(s->buf);
    memset(s, 0, sizeof *s);
}

void oc_share_rx_free(oc_share_rx *r) {
    for (int i = 0; i < OC_SHARE_SLOTS; i++) slot_drop(&r->slots[i]);
    r->handed = -1;
}

void oc_share_rx_need_key(oc_share_rx *r) { r->need_key = 1; }

static oc_share_slot *slot_of(oc_share_rx *r, uint32_t frame) {
    for (int i = 0; i < OC_SHARE_SLOTS; i++) if (r->slots[i].used && r->slots[i].frame == frame) return &r->slots[i];
    return NULL;
}

/* A slot for `frame`: a free one, or the oldest if `frame` is newer than it. The
 * oldest may be one still needed, in which case only a keyframe can follow. */
static oc_share_slot *slot_new(oc_share_rx *r, uint32_t frame, int64_t now_ms) {
    oc_share_slot *s = NULL, *old = NULL;
    for (int i = 0; i < OC_SHARE_SLOTS && !s; i++) if (!r->slots[i].used && i != r->handed) s = &r->slots[i];
    if (!s) {
        for (int i = 0; i < OC_SHARE_SLOTS; i++)
            if (i != r->handed && (!old || r->slots[i].frame < old->frame)) old = &r->slots[i];
        if (!old || old->frame > frame) return NULL;
        if (r->started && old->frame >= r->next_frame) r->need_key = 1;
        slot_drop(old);
        s = old;
    }
    s->used = 1;
    s->frame = frame;
    s->first_ms = now_ms;
    return s;
}

static int complete(const oc_share_slot *s) { return s->used && s->nfrags && s->have == s->nfrags; }

int oc_share_rx_put(oc_share_rx *r, const uint8_t *body, size_t len, int64_t now_ms) {
    if (len < OC_SHARE_VIDEO_HDR - 1 + 1) return -1;
    uint32_t frame = get32(body), frag = get16(body + 4), nfrags = get16(body + 6);
    int key = body[8] & 1;
    uint32_t w = get16(body + 9), h = get16(body + 11);
    const uint8_t *bytes = body + OC_SHARE_VIDEO_HDR - 1;
    size_t n = len - (OC_SHARE_VIDEO_HDR - 1);
    if (nfrags == 0 || nfrags > OC_SHARE_MAX_FRAGS || frag >= nfrags || n > OC_SHARE_FRAG ||
        (frag + 1 < nfrags && n != OC_SHARE_FRAG) || w < 2 || h < 2 || (w & 1) || (h & 1) ||
        w > OC_SHARE_MAX_DIM || h > OC_SHARE_MAX_DIM)
        return -1;
    if (r->started && frame < r->next_frame) { r->dups++; return -1; }   /* late, or a resend not needed */
    /* Frames between the newest so far and this one were sent and not seen: note
     * them, so they can be asked for. */
    if (r->started && r->have_top && frame > r->top_frame + 1) {
        for (uint32_t f = r->top_frame + 1; f < frame && f - r->top_frame <= 8; f++)
            if (f >= r->next_frame && !slot_of(r, f)) slot_new(r, f, now_ms);
    }
    if (!r->have_top || frame > r->top_frame) { r->top_frame = frame; r->have_top = 1; }
    oc_share_slot *s = slot_of(r, frame);
    if (!s && !(s = slot_new(r, frame, now_ms))) return -1;
    if (!s->nfrags) {
        if (!(s->buf = malloc((size_t)nfrags * OC_SHARE_FRAG))) { slot_drop(s); return -1; }
        s->nfrags = (uint16_t)nfrags;
        s->keyframe = (uint8_t)key;
        s->width = (uint16_t)w;
        s->height = (uint16_t)h;
    } else if (s->nfrags != nfrags || s->width != w || s->height != h || s->keyframe != key) {
        return -1;
    }
    if (s->got[frag / 8] & (1u << (frag % 8))) { r->dups++; return 0; }
    s->got[frag / 8] |= (uint8_t)(1u << (frag % 8));
    memcpy(s->buf + (size_t)frag * OC_SHARE_FRAG, bytes, n);
    if (frag + 1 == nfrags) s->last_len = (uint16_t)n;
    s->have++;
    r->frags++;
    return 0;
}

int oc_share_rx_next(oc_share_rx *r, const uint8_t **data, size_t *len, int *keyframe,
                     int *width, int *height, uint32_t *frame) {
    if (r->handed >= 0) { slot_drop(&r->slots[r->handed]); r->handed = -1; }
    oc_share_slot *s = r->started && !r->need_key ? slot_of(r, r->next_frame) : NULL;
    if (!s || !complete(s)) {
        /* Nothing in order: start, or start again, at the oldest complete keyframe
         * still to come. */
        oc_share_slot *k = NULL;
        for (int i = 0; i < OC_SHARE_SLOTS; i++) {
            oc_share_slot *c = &r->slots[i];
            if (!complete(c) || !c->keyframe || (r->started && c->frame < r->next_frame)) continue;
            if (!k || c->frame < k->frame) k = c;
        }
        if (!k) return 0;
        if (r->started && k->frame > r->next_frame) r->skipped += k->frame - r->next_frame;
        for (int i = 0; i < OC_SHARE_SLOTS; i++)
            if (r->slots[i].used && r->slots[i].frame < k->frame) slot_drop(&r->slots[i]);
        r->started = 1;
        r->need_key = 0;
        r->next_frame = k->frame;
        s = k;
    }
    *data = s->buf;
    *len = (size_t)(s->nfrags - 1) * OC_SHARE_FRAG + s->last_len;
    *keyframe = s->keyframe;
    *width = s->width;
    *height = s->height;
    *frame = s->frame;
    r->handed = (int)(s - r->slots);
    r->next_frame = s->frame + 1;
    r->frames++;
    return 1;
}

void oc_share_rx_poll(oc_share_rx *r, int64_t now_ms, oc_share_emit emit, void *ctx) {
    uint8_t pkt[OC_SHARE_CTL_MAX];
    if (r->born_ms < 0) r->born_ms = now_ms;
    /* Ask again for what went missing. */
    for (int i = 0; i < OC_SHARE_SLOTS; i++) {
        oc_share_slot *s = &r->slots[i];
        if (!s->used || i == r->handed || complete(s) || s->nacks >= 3) continue;
        if (r->started && s->frame < r->next_frame) continue;
        if (now_ms - (s->nacks ? s->nacked_ms : s->first_ms) < OC_SHARE_NACK_MS) continue;
        s->nacks++;
        s->nacked_ms = now_ms;
        r->nacks++;
        if (!s->nfrags) { emit(ctx, pkt, oc_share_ctl_nack(pkt, r->sharer, s->frame, NULL, 0)); continue; }
        uint16_t miss[64];
        int n = 0, sent = 0;
        for (uint32_t f = 0; f < s->nfrags; f++) {
            if (s->got[f / 8] & (1u << (f % 8))) continue;
            miss[n++] = (uint16_t)f;
            if (n == 64) {
                if (++sent > 4) { n = 0; emit(ctx, pkt, oc_share_ctl_nack(pkt, r->sharer, s->frame, NULL, 0)); break; }
                emit(ctx, pkt, oc_share_ctl_nack(pkt, r->sharer, s->frame, miss, n));
                n = 0;
            }
        }
        if (n) emit(ctx, pkt, oc_share_ctl_nack(pkt, r->sharer, s->frame, miss, n));
    }
    /* A frame the next one depends on that has not come in time is given up:
     * nothing after it decodes until a keyframe. */
    if (r->started && !r->need_key && r->have_top && r->top_frame >= r->next_frame) {
        oc_share_slot *s = slot_of(r, r->next_frame);
        if (!s) s = slot_new(r, r->next_frame, now_ms);
        if (s && !complete(s) && now_ms - s->first_ms >= OC_SHARE_GIVEUP_MS) {
            r->need_key = 1;
            r->skipped++;
        }
    }
    /* No keyframe to start from: ask for one, unless one is on its way -- or,
     * just arrived, it may be: a share starts with one. */
    if ((!r->started && now_ms - r->born_ms >= OC_SHARE_JOIN_MS) || (r->started && r->need_key)) {
        int coming = 0;
        for (int i = 0; i < OC_SHARE_SLOTS; i++) {
            const oc_share_slot *s = &r->slots[i];
            if (s->used && s->keyframe && (!r->started || s->frame >= r->next_frame) &&
                now_ms - s->first_ms < OC_SHARE_GIVEUP_MS) coming = 1;
        }
        if (!coming && (!r->pli_any || now_ms - r->pli_ms >= OC_SHARE_PLI_MS)) {
            emit(ctx, pkt, oc_share_ctl_pli(pkt, r->sharer));
            r->pli_ms = now_ms;
            r->pli_any = 1;
            r->plis++;
        }
    }
}

/* ---- what to send ------------------------------------------------------------------ */

uint64_t oc_share_frame_hash(const uint8_t *const plane[3], const int stride[3], int width, int height) {
    uint64_t h = 1469598103934665603ull;
    for (int p = 0; p < 3; p++) {
        int w = p ? width / 2 : width, rows = p ? height / 2 : height;
        for (int y = 0; y < rows; y++) {
            const uint8_t *row = plane[p] + (size_t)y * stride[p];
            int x = 0;
            for (; x + 8 <= w; x += 8) {
                uint64_t v;
                memcpy(&v, row + x, 8);
                h = (h ^ v) * 1099511628211ull;
            }
            for (; x < w; x++) h = (h ^ row[x]) * 1099511628211ull;
        }
    }
    return h;
}

void oc_share_pick_size(int src_w, int src_h, int kbps, int *small, int *w, int *h) {
    if (*small && kbps > 1000) *small = 0;
    else if (!*small && kbps < 600) *small = 1;
    *w = src_w & ~1;
    *h = src_h & ~1;
    if (*small && (src_w > 1280 || src_h > 720)) {
        int fw = 1280, fh = (int)((int64_t)1280 * src_h / src_w);
        if (fh > 720) { fh = 720; fw = (int)((int64_t)720 * src_w / src_h); }
        *w = fw & ~1;
        *h = fh & ~1;
    }
}
