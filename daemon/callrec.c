/* The Recorder -- see callrec.h and docs/details/CALLS.md §8. */

#include "callrec.h"

#include "e2e_hpke.h"
#include "e2e_sframe.h"
#include "oc_share.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define REC_KEYS   96                       /* (sender, epoch) keys held: two epochs of a full call */
#define REC_CAMS   ((int)OC_MAX_CALL_PARTICIPANTS)
#define REC_PT_MAX 1500
/* A packet under a key not yet had -- a device's first after it came, which
 * can be relayed before its key reaches here -- is held this long, this many
 * at most, and opened when the key comes. */
#define REC_HELD    32
#define REC_HELD_MS 2000

typedef struct {
    int              used;
    uint64_t         kid, user;
    uint32_t         epoch;
    oc_sframe_key    key;
    oc_sframe_replay rw;
} rkey;

typedef struct {
    int          used;
    uint64_t     user;
    oc_share_rx  rx;
    FILE        *f;
} rcam;

typedef struct { uint64_t user; FILE *f; } raudio;

struct oc_callrec {
    uint64_t        call_id, recording_id;
    char            dir[1024];
    uint8_t         sk[OC_X25519_LEN], pk[OC_X25519_LEN];
    uint32_t        epoch;
    uint8_t         slot;
    oc_callrec_part parts[OC_MAX_CALL_PARTICIPANTS];
    int             n_parts;
    oc_sframe_key   tx;                     /* its own, for its control packets */
    uint64_t        tx_ctr;
    uint16_t        seq;
    rkey            keys[REC_KEYS];
    uint64_t        sharer;
    int             share_on;
    oc_share_rx     share;
    FILE           *share_f, *timeline;
    rcam            cams[REC_CAMS];
    raudio          audio[OC_MAX_CALL_PARTICIPANTS * 4];
    int             n_audio;
    oc_callrec_stats st;
    FILE           *people;                 /* who each person is, for the picture */
    uint64_t        known[OC_MAX_CALL_PARTICIPANTS * 8];
    int             n_known;
    struct { uint64_t sender, kid, at; uint8_t stream; size_t len; uint8_t *ct; } held[REC_HELD];
    int             n_held;
};

static pthread_mutex_t g_last_mu = PTHREAD_MUTEX_INITIALIZER;
static oc_callrec_stats g_last;

static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v); }
static void be64(uint8_t *p, uint64_t v) { be32(p, (uint32_t)(v >> 32)); be32(p + 4, (uint32_t)v); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* The HPKE info a sealed key is bound to (CALLS.md §5.3): the client's, byte
 * for byte -- the call, the epoch, who sealed it and for whom. */
#define INFO_LEN (21u + 8u + 4u + 8u + 8u)
static void key_info(uint64_t call_id, uint32_t epoch, uint64_t sender, uint64_t recipient, uint8_t out[INFO_LEN]) {
    static const char label[] = "OpenChime call key v1";
    memcpy(out, label, sizeof label - 1);
    be64(out + 21, call_id);
    be32(out + 29, epoch);
    be64(out + 33, sender);
    be64(out + 41, recipient);
}

static FILE *spool_open(oc_callrec *r, const char *name) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", r->dir, name);
    return fopen(path, "ab");
}

static void mkdirs(const char *dir) {
    char p[1024];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = '\0'; mkdir(p, 0700); *s = '/'; }
    mkdir(p, 0700);
}

oc_callrec *oc_callrec_start(uint64_t call_id, uint64_t recording_id, const char *spool_dir, uint64_t now_ms) {
    if (!spool_dir || !*spool_dir) return NULL;
    oc_callrec *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->call_id = call_id;
    r->recording_id = recording_id;
    snprintf(r->dir, sizeof r->dir, "%s", spool_dir);
    r->st.call_id = call_id;
    mkdirs(r->dir);
    struct stat sb;
    if (stat(r->dir, &sb) != 0 || !S_ISDIR(sb.st_mode) || oc_x25519_keypair(r->sk, r->pk) != 0) {
        oc_e2e_wipe(r, sizeof *r);
        free(r);
        return NULL;
    }
    FILE *m = spool_open(r, "meta");
    r->timeline = spool_open(r, "timeline");
    r->people = spool_open(r, "people");
    if (!m || !r->timeline || !r->people) {
        if (m) fclose(m);
        if (r->timeline) fclose(r->timeline);
        if (r->people) fclose(r->people);
        oc_e2e_wipe(r, sizeof *r);
        free(r);
        return NULL;
    }
    uint8_t h[6 + 24];
    memcpy(h, "OCREC1", 6);
    be64(h + 6, call_id); be64(h + 14, recording_id); be64(h + 22, now_ms);
    fwrite(h, 1, sizeof h, m);
    fclose(m);
    return r;
}

const uint8_t *oc_callrec_public_key(const oc_callrec *r) { return r->pk; }

static const oc_callrec_part *part_of(const oc_callrec *r, uint64_t user) {
    for (int i = 0; i < r->n_parts; i++) if (r->parts[i].user == user) return &r->parts[i];
    return NULL;
}

static void timeline(oc_callrec *r, uint64_t at, uint8_t kind, uint64_t user) {
    uint8_t b[17];
    be64(b, at); b[8] = kind; be64(b + 9, user);
    if (r->timeline) { fwrite(b, 1, sizeof b, r->timeline); fflush(r->timeline); }
}

void oc_callrec_roster(oc_callrec *r, uint32_t epoch, uint8_t my_slot,
                       const oc_callrec_part *parts, int n, const oc_callrec_io *io) {
    if (!r || n < 0 || n > (int)OC_MAX_CALL_PARTICIPANTS) return;
    int new_epoch = epoch != r->epoch;
    memcpy(r->parts, parts, (size_t)n * sizeof *parts);
    r->n_parts = n;
    r->slot = my_slot;
    if (!new_epoch) return;
    if (!r->st.first_epoch) r->st.first_epoch = epoch;
    r->epoch = epoch;
    /* Keys older than the one before are of no use: wiped. */
    for (int i = 0; i < REC_KEYS; i++)
        if (r->keys[i].used && r->keys[i].epoch + 1 < epoch) {
            oc_sframe_key_wipe(&r->keys[i].key);
            memset(&r->keys[i], 0, sizeof r->keys[i]);
        }
    /* Its own key for the epoch, sealed to everyone else. */
    uint8_t base[16];
    if (oc_e2e_random(base, sizeof base) != 0) return;
    oc_sframe_key_wipe(&r->tx);
    if (oc_sframe_key_init(&r->tx, ((uint64_t)epoch << 8) | my_slot, base, sizeof base) != 0) {
        oc_e2e_wipe(base, sizeof base);
        return;
    }
    r->tx_ctr = 0;
    for (int i = 0; io && io->key_for && i < n; i++) {
        if (parts[i].user == OC_CALL_RECORDER_ID) continue;
        uint8_t info[INFO_LEN], sealed[OC_CALL_SEALED_LEN];
        key_info(r->call_id, epoch, OC_CALL_RECORDER_ID, parts[i].user, info);
        if (oc_hpke_seal_auth(parts[i].key, r->sk, info, sizeof info, NULL, 0, base, sizeof base,
                              sealed, sealed + OC_X25519_LEN) == 0)
            io->key_for(io->ctx, r->call_id, epoch, parts[i].user, sealed);
    }
    oc_e2e_wipe(base, sizeof base);
}

void oc_callrec_key(oc_callrec *r, uint64_t sender, uint32_t epoch, const uint8_t *sealed, size_t len) {
    if (!r || len != OC_CALL_SEALED_LEN || epoch > r->epoch || epoch + 1 < r->epoch) return;
    const oc_callrec_part *p = part_of(r, sender);
    if (!p || sender == OC_CALL_RECORDER_ID) return;
    uint64_t kid = ((uint64_t)epoch << 8) | p->slot;
    for (int i = 0; i < REC_KEYS; i++) if (r->keys[i].used && r->keys[i].kid == kid) return;   /* once */
    uint8_t info[INFO_LEN], base[16];
    key_info(r->call_id, epoch, sender, OC_CALL_RECORDER_ID, info);
    if (oc_hpke_open_auth(sealed, r->sk, p->key, info, sizeof info, NULL, 0,
                          sealed + OC_X25519_LEN, OC_CALL_SEALED_LEN - OC_X25519_LEN, base) != 0) return;
    rkey *k = NULL;
    for (int i = 0; i < REC_KEYS && !k; i++) if (!r->keys[i].used) k = &r->keys[i];
    if (!k) {   /* full: the oldest goes */
        k = &r->keys[0];
        for (int i = 1; i < REC_KEYS; i++) if (r->keys[i].epoch < k->epoch) k = &r->keys[i];
        oc_sframe_key_wipe(&k->key);
    }
    memset(k, 0, sizeof *k);
    if (oc_sframe_key_init(&k->key, kid, base, sizeof base) == 0) {
        k->used = 1;
        k->kid = kid;
        k->user = sender;
        k->epoch = epoch;
    }
    oc_e2e_wipe(base, sizeof base);
}

/* The Recorder's own control packet, sealed and put into the call. */
typedef struct { oc_callrec *r; const oc_callrec_io *io; } emit_ctx;
static void emit(void *ctx, const uint8_t *pt, size_t len) {
    emit_ctx *e = ctx;
    oc_callrec *r = e->r;
    if (!r->tx.ready || !e->io || !e->io->inject) return;
    uint8_t stream = OC_CALL_STREAM_AUDIO, ct[REC_PT_MAX + OC_SFRAME_OVERHEAD];
    size_t ctlen = 0;
    if (oc_sframe_encrypt(&r->tx, r->tx_ctr, &stream, 1, pt, len, ct, sizeof ct, &ctlen) != 0) return;
    r->tx_ctr++;
    if (len >= 10 && pt[0] == OC_CALLPKT_CONTROL) {
        int kind = pt[9] & ~OC_CTL_CAMERA;
        if (kind == OC_SHARE_NACK) r->st.nacks++;
        if (kind == OC_SHARE_PLI) r->st.plis++;
    }
    e->io->inject(e->io->ctx, r->call_id, r->seq++, stream, ct, ctlen);
}

static rcam *cam_of(oc_callrec *r, uint64_t user, int make) {
    for (int i = 0; i < REC_CAMS; i++) if (r->cams[i].used && r->cams[i].user == user) return &r->cams[i];
    if (!make) return NULL;
    for (int i = 0; i < REC_CAMS; i++)
        if (!r->cams[i].used) {
            rcam *c = &r->cams[i];
            memset(c, 0, sizeof *c);
            c->used = 1;
            c->user = user;
            oc_share_rx_init_camera(&c->rx, user);
            return c;
        }
    return NULL;
}

static FILE *audio_file(oc_callrec *r, uint64_t user) {
    for (int i = 0; i < r->n_audio; i++) if (r->audio[i].user == user) return r->audio[i].f;
    if (r->n_audio == (int)(sizeof r->audio / sizeof r->audio[0])) return NULL;
    char name[48];
    snprintf(name, sizeof name, "audio.%llu", (unsigned long long)user);
    FILE *f = spool_open(r, name);
    if (f) { r->audio[r->n_audio].user = user; r->audio[r->n_audio].f = f; r->n_audio++; }
    return f;
}

/* Whole frames a reassembly has ready, into the spool. */
static void drain_camera(oc_callrec *r, rcam *c, uint64_t now) {
    const uint8_t *data; size_t len; int key, w, h; uint32_t frame;
    while (oc_share_rx_next(&c->rx, &data, &len, &key, &w, &h, &frame)) {
        uint32_t aframe; const uint8_t *vp9; size_t vn;
        if (oc_cam_frame_get(data, len, &aframe, &vp9, &vn) != 0) continue;
        if (!c->f) {
            char name[48];
            snprintf(name, sizeof name, "camera.%llu", (unsigned long long)c->user);
            c->f = spool_open(r, name);
        }
        if (!c->f) continue;
        uint8_t hdr[21];
        be64(hdr, now); be32(hdr + 8, aframe); hdr[12] = (uint8_t)(key ? 1 : 0);
        be16(hdr + 13, (uint32_t)w); be16(hdr + 15, (uint32_t)h); be32(hdr + 17, (uint32_t)vn);
        fwrite(hdr, 1, sizeof hdr, c->f);
        fwrite(vp9, 1, vn, c->f);
        r->st.camera_frames++;
    }
}

static void drain_share(oc_callrec *r, uint64_t now) {
    const uint8_t *data; size_t len; int key, w, h; uint32_t frame;
    while (oc_share_rx_next(&r->share, &data, &len, &key, &w, &h, &frame)) {
        if (!r->share_f) r->share_f = spool_open(r, "share");
        if (!r->share_f) continue;
        uint8_t hdr[25];
        be64(hdr, now); be64(hdr + 8, r->sharer); hdr[16] = (uint8_t)(key ? 1 : 0);
        be16(hdr + 17, (uint32_t)w); be16(hdr + 19, (uint32_t)h); be32(hdr + 21, (uint32_t)len);
        fwrite(hdr, 1, sizeof hdr, r->share_f);
        fwrite(data, 1, len, r->share_f);
        r->st.share_frames++;
    }
}

void oc_callrec_sharer(oc_callrec *r, uint64_t sharer, uint64_t now_ms) {
    if (!r || sharer == r->sharer) return;
    if (r->share_on) { oc_share_rx_free(&r->share); r->share_on = 0; timeline(r, now_ms, OC_CALLREC_TL_SHARE_OFF, r->sharer); }
    r->sharer = sharer;
    if (sharer) {
        oc_share_rx_init(&r->share, sharer);
        r->share_on = 1;
        timeline(r, now_ms, OC_CALLREC_TL_SHARE_ON, sharer);
    }
}

void oc_callrec_camera(oc_callrec *r, uint64_t user, int on, uint64_t now_ms) {
    if (!r) return;
    rcam *c = cam_of(r, user, 0);
    if (c && !on) {
        oc_share_rx_free(&c->rx);
        if (c->f) fclose(c->f);
        memset(c, 0, sizeof *c);
    }
    if (on && !c) cam_of(r, user, 1);
    timeline(r, now_ms, on ? OC_CALLREC_TL_CAMERA_ON : OC_CALLREC_TL_CAMERA_OFF, user);
}

int oc_callrec_knows(const oc_callrec *r, uint64_t user) {
    for (int i = 0; r && i < r->n_known; i++) if (r->known[i] == user) return 1;
    return 0;
}

/* A `people` record: user u64, name length u8, the name, key length u8, the key. */
void oc_callrec_person(oc_callrec *r, uint64_t user, const char *name, const char *avatar_key) {
    if (!r || !user || !r->people || oc_callrec_knows(r, user) ||
        r->n_known == (int)(sizeof r->known / sizeof r->known[0])) return;
    size_t nl = name ? strlen(name) : 0, kl = avatar_key ? strlen(avatar_key) : 0;
    if (nl > OC_CALLREC_NAME_MAX) nl = OC_CALLREC_NAME_MAX;
    if (kl > OC_CALLREC_AVKEY_MAX) kl = 0;            /* a key too long is no key */
    uint8_t h[9];
    be64(h, user);
    h[8] = (uint8_t)nl;
    fwrite(h, 1, sizeof h, r->people);
    if (nl) fwrite(name, 1, nl, r->people);
    uint8_t k = (uint8_t)kl;
    fwrite(&k, 1, 1, r->people);
    if (kl) fwrite(avatar_key, 1, kl, r->people);
    fflush(r->people);
    r->known[r->n_known++] = user;
}

void oc_callrec_presence(oc_callrec *r, uint64_t user, int joined, uint64_t now_ms) {
    if (r) timeline(r, now_ms, joined ? OC_CALLREC_TL_JOIN : OC_CALLREC_TL_LEAVE, user);
}

static void rec_open(oc_callrec *r, uint64_t sender, uint8_t stream, const uint8_t *ct, size_t len,
                     uint64_t now_ms, const oc_callrec_io *io, int held);

void oc_callrec_packet(oc_callrec *r, uint64_t sender, uint16_t seq, uint8_t stream,
                       const uint8_t *ct, size_t len, uint64_t now_ms, const oc_callrec_io *io) {
    (void)seq;
    if (!r || !len) return;
    r->st.packets++;
    rec_open(r, sender, stream, ct, len, now_ms, io, 0);
}

/* Open one packet and spool what it carries. One whose key has not come yet,
 * of an epoch since the Recorder came, is held for it (unless `held`: it
 * already was). */
static void rec_open(oc_callrec *r, uint64_t sender, uint8_t stream, const uint8_t *ct, size_t len,
                     uint64_t now_ms, const oc_callrec_io *io, int held) {
    uint64_t kid = 0, ctr;
    size_t hl;
    rkey *k = NULL;
    int hdr_ok = oc_sframe_header_decode(ct, len, &kid, &ctr, &hl) == 0;
    if (hdr_ok)
        for (int i = 0; i < REC_KEYS && !k; i++) if (r->keys[i].used && r->keys[i].kid == kid) k = &r->keys[i];
    if (hdr_ok && !k && !held && r->st.first_epoch && (uint32_t)(kid >> 8) >= r->st.first_epoch && r->n_held < REC_HELD) {
        uint8_t *copy = malloc(len);
        if (copy) {
            memcpy(copy, ct, len);
            r->held[r->n_held].sender = sender; r->held[r->n_held].kid = kid; r->held[r->n_held].at = now_ms;
            r->held[r->n_held].stream = stream; r->held[r->n_held].len = len; r->held[r->n_held].ct = copy;
            r->n_held++;
            return;
        }
    }
    uint8_t pt[REC_PT_MAX];
    size_t ptlen = 0;
    uint64_t got = 0;
    /* The key must be this sender's (the relay names who sent it), the counter
     * new, the packet authentic, and its type its stream's. */
    if (!k || k->user != sender || !oc_sframe_replay_ok(&k->rw, ctr) ||
        oc_sframe_decrypt(&k->key, &stream, 1, ct, len, pt, sizeof pt, &ptlen, &got) != 0 || ptlen < 2 ||
        (pt[0] == OC_CALLPKT_VIDEO ? OC_CALL_STREAM_SHARE :
         pt[0] == OC_CALLPKT_CAMERA ? OC_CALL_STREAM_CAMERA : OC_CALL_STREAM_AUDIO) != stream) {
        if (!k && (uint32_t)(kid >> 8) < r->st.first_epoch) r->st.before++;
        else r->st.unopened++;
        return;
    }
    oc_sframe_replay_mark(&k->rw, got);
    r->st.opened++;
    emit_ctx ec = { r, io };
    if (pt[0] == OC_CALLPKT_AUDIO && ptlen > 5 && ptlen - 5 <= 0xFFFF) {
        FILE *f = audio_file(r, sender);
        if (f) {
            uint8_t hdr[14];
            be64(hdr, now_ms); be32(hdr + 8, rd32(pt + 1)); be16(hdr + 12, (uint32_t)(ptlen - 5));
            fwrite(hdr, 1, sizeof hdr, f);
            fwrite(pt + 5, 1, ptlen - 5, f);
            r->st.audio_frames++;
        }
    } else if (pt[0] == OC_CALLPKT_VIDEO && r->share_on && sender == r->sharer) {
        oc_share_rx_put(&r->share, pt + 1, ptlen - 1, (int64_t)now_ms);
        drain_share(r, now_ms);
        oc_share_rx_poll(&r->share, (int64_t)now_ms, emit, &ec);
    } else if (pt[0] == OC_CALLPKT_CAMERA) {
        rcam *c = cam_of(r, sender, 1);
        if (c) {
            oc_share_rx_put(&c->rx, pt + 1, ptlen - 1, (int64_t)now_ms);
            drain_camera(r, c, now_ms);
            oc_share_rx_poll(&c->rx, (int64_t)now_ms, emit, &ec);
        }
    }
}

void oc_callrec_tick(oc_callrec *r, uint64_t now_ms, const oc_callrec_io *io) {
    if (!r) return;
    /* Held packets whose key has come are opened; one held too long is not. */
    for (int i = 0; i < r->n_held; ) {
        int have = 0;
        for (int k = 0; k < REC_KEYS && !have; k++) have = r->keys[k].used && r->keys[k].kid == r->held[i].kid;
        if (have || now_ms - r->held[i].at > REC_HELD_MS) {
            if (have) rec_open(r, r->held[i].sender, r->held[i].stream, r->held[i].ct, r->held[i].len, now_ms, io, 1);
            else r->st.unopened++;
            free(r->held[i].ct);
            r->held[i] = r->held[--r->n_held];
        } else {
            i++;
        }
    }
    emit_ctx ec = { r, io };
    if (r->share_on) { oc_share_rx_poll(&r->share, (int64_t)now_ms, emit, &ec); drain_share(r, now_ms); }
    for (int i = 0; i < REC_CAMS; i++)
        if (r->cams[i].used) { oc_share_rx_poll(&r->cams[i].rx, (int64_t)now_ms, emit, &ec); drain_camera(r, &r->cams[i], now_ms); }
}

void oc_callrec_get_stats(const oc_callrec *r, oc_callrec_stats *out) {
    if (r && out) *out = r->st;
}

void oc_callrec_stop(oc_callrec *r) {
    if (!r) return;
    if (r->share_on) oc_share_rx_free(&r->share);
    if (r->share_f) fclose(r->share_f);
    for (int i = 0; i < REC_CAMS; i++)
        if (r->cams[i].used) { oc_share_rx_free(&r->cams[i].rx); if (r->cams[i].f) fclose(r->cams[i].f); }
    for (int i = 0; i < r->n_audio; i++) if (r->audio[i].f) fclose(r->audio[i].f);
    if (r->timeline) fclose(r->timeline);
    if (r->people) fclose(r->people);
    for (int i = 0; i < r->n_held; i++) { free(r->held[i].ct); r->st.unopened++; }   /* never had its key */
    r->n_held = 0;
    for (int i = 0; i < REC_KEYS; i++) oc_sframe_key_wipe(&r->keys[i].key);
    oc_sframe_key_wipe(&r->tx);
    pthread_mutex_lock(&g_last_mu);
    g_last = r->st;
    pthread_mutex_unlock(&g_last_mu);
    oc_e2e_wipe(r, sizeof *r);
    free(r);
}

void oc_callrec_last_stats(oc_callrec_stats *out) {
    pthread_mutex_lock(&g_last_mu);
    if (out) *out = g_last;
    pthread_mutex_unlock(&g_last_mu);
}
