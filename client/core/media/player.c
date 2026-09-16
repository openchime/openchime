/* Playing a video message (oc_player.h).
 *
 * One worker thread owns the demuxed file, both decoders and the speaker ring.
 * The UI thread only flips requests under the mutex and copies the presented
 * frame out. */
#define _POSIX_C_SOURCE 200809L
#include "oc_player.h"
#include "oc_codec.h"
#include "oc_mp4.h"
#include "audio_dev.h"
#include "oc_thread.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define AUDIO_AHEAD     9600       /* keep ~200 ms queued at the speaker */
#define OPUS_PREROLL    3840       /* 80 ms decoded and discarded before a seek point */

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

struct oc_player {
    const uint8_t *file;
    oc_mp4_info    info;
    oc_audio_dev  *speaker;

    oc_mutex_t     mu;
    int            quit;
    oc_player_state state;
    int            want_play;
    int            seek_pending;
    uint32_t       seek_ms;
    int            decode_delay_ms;

    /* presented frame (under mu) */
    uint8_t       *bgra;
    int            fw, fh;
    size_t         bgra_cap;
    uint64_t       frame_seq;
    uint64_t       presented, dropped;
    uint32_t       position_ms;
    uint32_t       frame_ms;

    oc_thread_t    thread;
};

/* ---- the worker ---------------------------------------------------------------- */

typedef struct {
    oc_player  *p;
    oc_vp9dec  *vdec;
    oc_opusdec *adec;
    uint32_t    vnext;             /* next video sample to decode */
    uint32_t    anext;             /* next audio packet to decode */
    uint64_t    audio_discard;     /* decoded samples still to throw away */
    oc_frame    pending;           /* decoded, not yet presented */
    int         have_pending;
    /* The clock: movie time `base_us` at audio position `base_played`, or at
     * monotonic time `base_wall` when there is no speaker. */
    int64_t     base_us;
    uint64_t    base_played;
    int64_t     base_wall;
    int         running;           /* the clock is advancing */
    int         decode_delay_ms;   /* tests: copied from the player each turn */
    int64_t     last_present_wall; /* when a frame was last shown */
} worker;

static int64_t clock_us(worker *w) {
    if (!w->running) return w->base_us;
    if (w->p->speaker)
        return w->base_us + (int64_t)((oc_audio_playback_position(w->p->speaker) - w->base_played) * 1000000 / OC_OPUS_RATE);
    return w->base_us + (oc_media_clock_us() - w->base_wall);
}

static int64_t video_us(const oc_player *p, uint32_t i) {
    return (int64_t)(p->info.video.samples[i].dts * 1000000 / p->info.video.timescale);
}

/* Present `f`: convert into the shared BGRA buffer. */
static void present(oc_player *p, const oc_frame *f, int64_t pts_us) {
    size_t need = (size_t)f->width * (size_t)f->height * 4;
    oc_mutex_lock(&p->mu);
    if (need > p->bgra_cap) {
        uint8_t *b = realloc(p->bgra, need);
        if (!b) { oc_mutex_unlock(&p->mu); return; }
        p->bgra = b; p->bgra_cap = need;
    }
    oc_i420_to_bgra(f, p->bgra, f->width * 4);
    p->fw = f->width; p->fh = f->height;
    p->frame_seq++;
    p->presented++;
    p->frame_ms = (uint32_t)(pts_us / 1000);
    oc_mutex_unlock(&p->mu);
}

/* Decode the next video sample into `pending`. 0 decoded, 1 none left, -1 error. */
static int decode_video(worker *w) {
    oc_player *p = w->p;
    while (w->vnext < p->info.video.n_samples) {
        const oc_mp4_sample *s = &p->info.video.samples[w->vnext];
        int64_t pts = video_us(p, w->vnext);
        w->vnext++;
        oc_frame f;
        int rc = oc_vp9dec_decode(w->vdec, p->file + s->offset, s->size, pts, &f);
        if (w->decode_delay_ms > 0) sleep_ms(w->decode_delay_ms);
        if (rc < 0) return -1;
        if (rc == 1) continue;
        /* The decoder's frame is only valid until its next call: keep a copy. */
        if (!w->pending.plane[0] || w->pending.width != f.width || w->pending.height != f.height) {
            oc_frame_free(&w->pending);
            if (oc_frame_alloc(&w->pending, f.width, f.height) != 0) return -1;
        }
        oc_frame_copy(&w->pending, &f);
        w->have_pending = 1;
        return 0;
    }
    w->have_pending = 0;
    return 1;
}

static void fill_audio(worker *w) {
    oc_player *p = w->p;
    if (!p->speaker || !w->adec) return;
    int16_t pcm[5760];
    while (oc_audio_playback_queued(p->speaker) < AUDIO_AHEAD && w->anext < p->info.audio.n_samples) {
        const oc_mp4_sample *s = &p->info.audio.samples[w->anext++];
        int n = oc_opusdec_decode(w->adec, p->file + s->offset, s->size, pcm, (int)(sizeof pcm / sizeof *pcm));
        if (n <= 0) continue;
        int off = 0;
        if (w->audio_discard > 0) {
            off = w->audio_discard >= (uint64_t)n ? n : (int)w->audio_discard;
            w->audio_discard -= (uint64_t)off;
        }
        if (n > off) oc_audio_playback_write(p->speaker, pcm + off, (size_t)(n - off));
    }
}

/* Point the speaker path at movie time `at_us`: drop what is queued, restart the
 * decoder a little early so it has converged, and discard up to the target. */
static int audio_reposition(worker *w, int64_t at_us) {
    oc_player *p = w->p;
    if (!p->speaker || !w->adec) return 0;
    oc_audio_playback_flush(p->speaker);
    oc_opusdec_close(w->adec);
    w->adec = oc_opusdec_open(p->info.opus_channels);
    if (!w->adec) return -1;
    /* Decoded stream sample `target` is movie time `at_us`. */
    uint64_t target = (uint64_t)(at_us * p->info.audio.timescale / 1000000) + p->info.opus_preskip;
    uint64_t from = target > OPUS_PREROLL ? target - OPUS_PREROLL : 0;
    uint32_t a = 0;
    while (a + 1 < p->info.audio.n_samples && p->info.audio.samples[a + 1].dts <= from) a++;
    w->anext = a;
    w->audio_discard = target > p->info.audio.samples[a].dts ? target - p->info.audio.samples[a].dts : 0;
    w->base_played = oc_audio_playback_position(p->speaker);
    return 0;
}

/* Move every decoder and the clock to the keyframe at or before `ms`, and show it.
 * A read-aloud rendering has no video track (ARCH-111): there is no keyframe to
 * snap to and no picture to show, so the position is exactly what was asked and
 * only the audio moves. */
static int seek_to(worker *w, uint32_t ms) {
    oc_player *p = w->p;
    if (!p->info.video.present) {
        int64_t at_us = (int64_t)ms * 1000;
        w->have_pending = 0;
        if (audio_reposition(w, at_us) != 0) return -1;
        w->base_us = at_us;
        w->base_wall = oc_media_clock_us();
        oc_mutex_lock(&p->mu); p->position_ms = ms; oc_mutex_unlock(&p->mu);
        return 0;
    }
    uint64_t dts = (uint64_t)ms * p->info.video.timescale / 1000;
    int k = oc_mp4_keyframe_before(&p->info, dts);
    if (k < 0) k = 0;
    w->vnext = (uint32_t)k;
    w->have_pending = 0;
    if (decode_video(w) < 0) return -1;
    int64_t at_us = w->have_pending ? w->pending.pts_us : video_us(p, (uint32_t)k);

    if (audio_reposition(w, at_us) != 0) return -1;
    w->base_us = at_us;
    w->base_wall = oc_media_clock_us();
    oc_mutex_lock(&p->mu); p->position_ms = (uint32_t)(at_us / 1000); oc_mutex_unlock(&p->mu);
    if (w->have_pending) { present(p, &w->pending, at_us); w->have_pending = 0; if (decode_video(w) < 0) return -1; }
    return 0;
}

static void *player_main(void *arg) {
    oc_player *p = arg;
    worker w = { .p = p };
    w.vdec = p->info.video.present ? oc_vp9dec_open() : NULL;
    w.adec = p->info.audio.present ? oc_opusdec_open(p->info.opus_channels) : NULL;
    int err = (p->info.video.present && !w.vdec) || (p->info.audio.present && !w.adec) ||
              seek_to(&w, 0) != 0;
    int64_t end_us = (int64_t)p->info.duration_ms * 1000;

    while (!err) {
        oc_mutex_lock(&p->mu);
        int quit = p->quit, want = p->want_play, seek = p->seek_pending;
        uint32_t seek_ms = p->seek_ms;
        w.decode_delay_ms = p->decode_delay_ms;
        p->seek_pending = 0;
        oc_mutex_unlock(&p->mu);
        if (quit) break;

        if (seek) {
            int was = w.running;
            w.running = 0;
            if (seek_to(&w, seek_ms) != 0) { err = 1; break; }
            w.running = was;
            oc_mutex_lock(&p->mu);
            if (p->state == OC_PLAYER_ENDED) p->state = OC_PLAYER_PAUSED;
            oc_mutex_unlock(&p->mu);
        }
        if (want && !w.running) {
            oc_mutex_lock(&p->mu);
            int ended = p->state == OC_PLAYER_ENDED;
            oc_mutex_unlock(&p->mu);
            if (ended && seek_to(&w, 0) != 0) { err = 1; break; }
            w.running = 1;
            w.base_wall = oc_media_clock_us();
            if (p->speaker) w.base_played = oc_audio_playback_position(p->speaker);
            fill_audio(&w);
            oc_mutex_lock(&p->mu); p->state = OC_PLAYER_PLAYING; oc_mutex_unlock(&p->mu);
        } else if (!want && w.running) {
            /* Pausing is a seek to where the clock stands, without the keyframe
             * snap: stop the clock and drop what the speaker has queued. */
            int64_t now = clock_us(&w);
            w.running = 0;
            w.base_us = now;
            if (audio_reposition(&w, now) != 0) { err = 1; break; }
            oc_mutex_lock(&p->mu); if (p->state == OC_PLAYER_PLAYING) p->state = OC_PLAYER_PAUSED; oc_mutex_unlock(&p->mu);
        }

        if (!w.running) { sleep_ms(10); continue; }

        fill_audio(&w);
        int64_t now = clock_us(&w);
        oc_mutex_lock(&p->mu);
        p->position_ms = (uint32_t)((now < 0 ? 0 : now > end_us ? end_us : now) / 1000);
        oc_mutex_unlock(&p->mu);
        if (w.have_pending && w.pending.pts_us <= now) {
            int64_t next_us = w.vnext < p->info.video.n_samples ? video_us(p, w.vnext) : INT64_MAX;
            /* However late, a frame is shown at least every 100 ms, so a slow
             * decoder degrades to a low frame rate rather than a frozen picture. */
            int starved = oc_media_clock_us() - w.last_present_wall > 100000;
            if (next_us <= now && !starved) {
                /* Late by more than a frame, with the next also due: skip showing
                 * it. If the clock has passed a later keyframe, no amount of
                 * decoding in between is worth doing — jump there. */
                oc_mutex_lock(&p->mu); p->dropped++; oc_mutex_unlock(&p->mu);
                int k = oc_mp4_keyframe_before(&p->info, (uint64_t)now * p->info.video.timescale / 1000000);
                if (k >= 0 && (uint32_t)k >= w.vnext) {
                    oc_mutex_lock(&p->mu); p->dropped += (uint32_t)k - w.vnext; oc_mutex_unlock(&p->mu);
                    w.vnext = (uint32_t)k;
                }
            } else {
                present(p, &w.pending, w.pending.pts_us);
                w.last_present_wall = oc_media_clock_us();
            }
            if (decode_video(&w) < 0) { err = 1; break; }
            continue;
        }
        if (!w.have_pending && now >= end_us) {
            w.running = 0;
            w.base_us = end_us;
            oc_mutex_lock(&p->mu);
            p->state = OC_PLAYER_ENDED;
            p->want_play = 0;
            p->position_ms = p->info.duration_ms;
            oc_mutex_unlock(&p->mu);
            continue;
        }
        int64_t wait_ms = w.have_pending ? (w.pending.pts_us - now) / 1000 : 10;
        sleep_ms(wait_ms < 1 ? 1 : wait_ms > 10 ? 10 : (int)wait_ms);
    }

    if (err) { oc_mutex_lock(&p->mu); p->state = OC_PLAYER_ERROR; oc_mutex_unlock(&p->mu); }
    oc_frame_free(&w.pending);
    oc_vp9dec_close(w.vdec);
    oc_opusdec_close(w.adec);
    return NULL;
}

/* ---- public ------------------------------------------------------------------------ */

oc_player *oc_player_open(const uint8_t *mp4, size_t len) {
    oc_player *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    if (oc_mp4_parse(mp4, len, &p->info) != 0) { free(p); return NULL; }
    p->file = mp4;
    if (p->info.audio.present) {
        int aerr;
        p->speaker = oc_audio_playback_open(NULL, OC_OPUS_RATE, 1, &aerr);
    }
    oc_mutex_init(&p->mu);
    p->state = OC_PLAYER_PAUSED;
    if (oc_thread_create(&p->thread, player_main, p) != 0) {
        oc_audio_close(p->speaker);
        oc_mp4_info_free(&p->info);
        oc_mutex_destroy(&p->mu);
        free(p);
        return NULL;
    }
    return p;
}

void oc_player_play(oc_player *p)  { oc_mutex_lock(&p->mu); p->want_play = 1; oc_mutex_unlock(&p->mu); }
void oc_player_pause(oc_player *p) { oc_mutex_lock(&p->mu); p->want_play = 0; oc_mutex_unlock(&p->mu); }

void oc_player_seek(oc_player *p, uint32_t ms) {
    oc_mutex_lock(&p->mu);
    p->seek_ms = ms > p->info.duration_ms ? p->info.duration_ms : ms;
    p->seek_pending = 1;
    oc_mutex_unlock(&p->mu);
}

void oc_player_volume(oc_player *p, float gain) {
    if (p->speaker) oc_audio_playback_volume(p->speaker, gain);
}

void oc_player_status_get(oc_player *p, oc_player_status *st) {
    oc_mutex_lock(&p->mu);
    st->state = p->state;
    st->position_ms = p->position_ms;
    st->frame_ms = p->frame_ms;
    st->duration_ms = p->info.duration_ms;
    st->width = p->info.width;
    st->height = p->info.height;
    st->has_audio = p->speaker != NULL;
    st->presented = p->presented;
    st->dropped = p->dropped;
    oc_mutex_unlock(&p->mu);
}

int oc_player_frame(oc_player *p, uint8_t *bgra, size_t cap, int *w, int *h, uint64_t *seq) {
    /* A read-aloud rendering has no picture at all (ARCH-111), so a caller
     * playing one asks with nothing to fill in and is told "nothing new". */
    if (!p->info.video.present || !bgra || !seq) {
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    oc_mutex_lock(&p->mu);
    int rc = 0;
    *w = p->fw; *h = p->fh;
    if (p->frame_seq != *seq && p->bgra) {
        size_t need = (size_t)p->fw * (size_t)p->fh * 4;
        if (cap < need) rc = -1;
        else { memcpy(bgra, p->bgra, need); *seq = p->frame_seq; rc = 1; }
    }
    oc_mutex_unlock(&p->mu);
    return rc;
}

void oc_player_test_decode_delay(oc_player *p, int ms) {
    oc_mutex_lock(&p->mu); p->decode_delay_ms = ms; oc_mutex_unlock(&p->mu);
}

void oc_player_close(oc_player *p) {
    if (!p) return;
    oc_mutex_lock(&p->mu); p->quit = 1; oc_mutex_unlock(&p->mu);
    oc_thread_join(p->thread);
    oc_audio_close(p->speaker);
    oc_mp4_info_free(&p->info);
    free(p->bgra);
    oc_mutex_destroy(&p->mu);
    free(p);
}
