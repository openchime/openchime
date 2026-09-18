/* Recording a video message (oc_recorder.h).
 *
 * Two threads. The capture thread pulls camera frames, keeps the latest for the
 * preview and, while recording, copies each into a bounded queue. The encode
 * thread takes frames from that queue, reads the microphone ring in 20 ms
 * blocks, encodes both and appends the samples to the MP4 writer. A full queue
 * drops the new frame rather than growing, so a slow encoder shows up as
 * dropped frames and queue depth, never as unbounded memory. */
#define _POSIX_C_SOURCE 200809L
#include "oc_recorder.h"
#include "oc_capture.h"
#include "oc_codec.h"
#include "oc_mp4.h"
#include "oc_processor.h"
#include "audio_dev.h"
#include "oc_thread.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../../../third_party/stb/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define QUEUE_FRAMES   24                 /* ~800 ms at 30 fps */
#define BEHIND_US      500000             /* step down past this much queued */

/* The size encoded for a camera frame of cam_w×cam_h when `want_h` is asked
 * for: the camera's own size when it is no taller, else scaled down to that
 * height keeping its shape. Even dimensions throughout. */
static void encode_size(int cam_w, int cam_h, int want_h, int *w, int *h) {
    if (want_h <= 0 || cam_h <= want_h) { *w = cam_w; *h = cam_h; return; }
    *h = want_h & ~1;
    *w = (int)((long)cam_w * want_h / cam_h) & ~1;
}

/* One size down when the encoder falls behind: 1080p to 720p, 720p to 360p;
 * 360p has nowhere to go. */
static int step_height(int enc_h) { return enc_h > 720 ? 720 : enc_h > 360 ? 360 : 0; }

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* Timestamped audio waiting to be mixed: samples[0] was captured at pts_us. */
#define MIX_BUF (OC_OPUS_RATE * 2)
typedef struct {
    oc_audio_dev *dev;
    int16_t       buf[MIX_BUF];
    size_t        n;
    int64_t       pts_us;
} mix_src;

struct oc_recorder {
    oc_capture   *cam;                  /* the camera, or the screen or window */
    oc_audio_dev *mic;
    /* A screen recording: the camera in its box, the computer's sound. */
    int           screen, corner, source_gone;
    oc_capture   *inset;
    oc_frame      inset_latest, comp;   /* under mu */
    int           inset_have;
    oc_thread_t   inset_thread;
    int           inset_started;
    oc_audio_dev *loop;
    mix_src       mic_s, loop_s;        /* encode thread */
    const oc_audio_processor *proc;
    void         *proc_state;
    int64_t       a_t;                  /* where the next mixed block starts */
    int64_t       last_video_us;
    int           width, height, fps;   /* the camera's frames */
    int           want_h;               /* the chosen quality */
    int           enc_w, enc_h;         /* what is encoded now */
    uint64_t      max_bytes;
    uint32_t      cap_ms;

    oc_mutex_t    mu;
    oc_cond_t     cv;
    int           quit;
    oc_rec_state  state;
    int           stop_requested;

    /* preview (under mu) */
    oc_frame      latest;
    uint64_t      latest_seq;

    /* frame queue (under mu): a ring of preallocated frames */
    oc_frame      pool[QUEUE_FRAMES];
    int           q_head, q_len;
    uint64_t      dropped;

    /* recording (encode thread; status fields under mu) */
    int64_t       t0_us;                   /* the first recorded frame's capture time */
    uint32_t      elapsed_ms;
    int           stepped_down;
    int           has_audio;
    oc_rec_result result;

    oc_thread_t   cap_thread, enc_thread;
    int           threads_started;
};

static uint32_t cap_from_env(uint32_t cap) {
    const char *t = getenv("OPENCHIME_TEST_VIDEO_CAP_MS");
    if (t && *t) {
        long v = strtol(t, NULL, 10);
        if (v > 0 && v <= (long)OC_RECORDER_CAP_MS) return (uint32_t)v;
    }
    return cap;
}

/* ---- capture threads ------------------------------------------------------------ */

/* The camera of a screen recording: its latest frame, for the box. */
static void *inset_main(void *arg) {
    oc_recorder *r = arg;
    for (;;) {
        oc_frame f;
        int rc = oc_capture_next(r->inset, &f, 100);
        oc_mutex_lock(&r->mu);
        if (r->quit || rc < 0) { oc_mutex_unlock(&r->mu); break; }   /* a lost camera keeps its last frame */
        if (rc == 1) {
            if (!r->inset_have || r->inset_latest.width != f.width || r->inset_latest.height != f.height) {
                oc_frame_free(&r->inset_latest);
                r->inset_have = oc_frame_alloc(&r->inset_latest, f.width, f.height) == 0;
            }
            if (r->inset_have) oc_frame_copy(&r->inset_latest, &f);
        }
        oc_mutex_unlock(&r->mu);
    }
    return NULL;
}

static void *capture_main(void *arg) {
    oc_recorder *r = arg;
    for (;;) {
        oc_frame f;
        int rc = oc_capture_next(r->cam, &f, 100);
        oc_mutex_lock(&r->mu);
        if (r->quit) { oc_mutex_unlock(&r->mu); break; }
        if (rc < 0) {
            /* A window closed while it was recorded: keep what came before it,
             * as Stop would. Anything else is a failure. */
            if (rc == OC_CAP_GONE && r->state == OC_REC_RECORDING) {
                r->source_gone = 1;
                r->stop_requested = 1;
            } else if (r->state == OC_REC_RECORDING || r->state == OC_REC_PREVIEW) {
                if (rc == OC_CAP_GONE) r->source_gone = 1;
                r->state = OC_REC_ERROR;
            }
            oc_cond_signal(&r->cv);
            oc_mutex_unlock(&r->mu);
            break;
        }
        /* Until recording starts this thread is the microphone ring's only
         * reader, and discards what it reads so the ring never overflows; from
         * RECORDING on (a state never left for PREVIEW) the encode thread is.
         * The computer's sound likewise. */
        if (r->state == OC_REC_PREVIEW) {
            int16_t discard[960];
            if (r->mic) while (oc_audio_capture_read(r->mic, discard, 960, NULL) > 0) {}
            if (r->loop) while (oc_audio_capture_read(r->loop, discard, 960, NULL) > 0) {}
        }
        /* The camera boxed into its corner, before anyone -- preview or
         * encoder -- sees the frame. */
        if (rc == 1 && r->inset && r->inset_have && r->comp.plane[0] &&
            f.width == r->comp.width && f.height == r->comp.height) {
            oc_frame_copy(&r->comp, &f);
            oc_i420_inset(&r->comp, &r->inset_latest, r->corner);
            f = r->comp;
        }
        if (rc == 1 && f.width == r->width && f.height == r->height) {
            oc_frame_copy(&r->latest, &f);
            r->latest_seq++;
            if (r->state == OC_REC_RECORDING && !r->stop_requested) {
                if (r->q_len == QUEUE_FRAMES) {
                    r->dropped++;
                } else {
                    oc_frame *slot = &r->pool[(r->q_head + r->q_len) % QUEUE_FRAMES];
                    oc_frame_copy(slot, &f);
                    r->q_len++;
                    oc_cond_signal(&r->cv);
                }
            }
        }
        oc_mutex_unlock(&r->mu);
    }
    return NULL;
}

/* ---- encode thread -------------------------------------------------------------- */

typedef struct {
    oc_recorder   *r;
    oc_mp4_writer *mp4;
    int            failed;
} enc_ctx;

static void emit_video(void *ctx, const oc_packet *p) {
    enc_ctx *e = ctx;
    if (oc_mp4_write_video(e->mp4, p->data, p->len, p->pts_us, p->keyframe) != 0) e->failed = 1;
}

/* Encode every whole 20 ms block of microphone audio waiting (a flush pads and
 * encodes the remainder too). Samples from before the first video frame are
 * discarded, so both tracks start at t0. */
static void pump_audio(oc_recorder *r, oc_opusenc *oe, enc_ctx *e, int16_t *acc, size_t *acc_n,
                       int flush) {
    int16_t buf[OC_OPUS_FRAME];
    uint8_t pkt[OC_OPUS_MAX_PACKET];
    for (;;) {
        int64_t pts;
        size_t want = OC_OPUS_FRAME - *acc_n;
        size_t got = oc_audio_capture_read(r->mic, buf, want, &pts);
        if (got == 0) break;
        size_t skip = 0;
        if (*acc_n == 0 && pts < r->t0_us) {
            int64_t early = (r->t0_us - pts) * OC_OPUS_RATE / 1000000;
            skip = early >= (int64_t)got ? got : (size_t)early;
        }
        memcpy(acc + *acc_n, buf + skip, (got - skip) * sizeof *acc);
        *acc_n += got - skip;
        if (*acc_n == OC_OPUS_FRAME) {
            int n = oc_opusenc_encode(oe, acc, pkt, sizeof pkt);
            if (n < 0 || oc_mp4_write_audio(e->mp4, pkt, (size_t)n, OC_OPUS_FRAME) != 0) e->failed = 1;
            *acc_n = 0;
        }
    }
    if (flush && *acc_n > 0) {
        memset(acc + *acc_n, 0, (OC_OPUS_FRAME - *acc_n) * sizeof *acc);
        int n = oc_opusenc_encode(oe, acc, pkt, sizeof pkt);
        if (n < 0 || oc_mp4_write_audio(e->mp4, pkt, (size_t)n, OC_OPUS_FRAME) != 0) e->failed = 1;
        *acc_n = 0;
    }
}

/* ---- the computer's sound, mixed with the microphone ------------------------ */

/* Read what is waiting into a source's buffer. A device's samples are
 * contiguous, so only an empty buffer takes a new timestamp. */
static void mix_fill(mix_src *s) {
    if (!s->dev) return;
    while (s->n < MIX_BUF) {
        int64_t pts = 0;
        size_t got = oc_audio_capture_read(s->dev, s->buf + s->n, MIX_BUF - s->n, &pts);
        if (got == 0) break;
        if (s->n == 0) s->pts_us = pts;
        s->n += got;
    }
}

/* Drop what is older than `t`. */
static void mix_drop_before(mix_src *s, int64_t t) {
    if (s->n == 0 || s->pts_us >= t) return;
    size_t k = (size_t)((t - s->pts_us) * OC_OPUS_RATE / 1000000);
    if (k >= s->n) { s->n = 0; return; }
    memmove(s->buf, s->buf + k, (s->n - k) * sizeof *s->buf);
    s->n -= k;
    s->pts_us += (int64_t)k * 1000000 / OC_OPUS_RATE;
}

/* Samples available from `t` on; a stretch before the first one is silence. */
static size_t mix_avail(mix_src *s, int64_t t) {
    mix_drop_before(s, t);
    if (s->n == 0) return 0;
    size_t lead = s->pts_us > t ? (size_t)((s->pts_us - t) * OC_OPUS_RATE / 1000000) : 0;
    return lead + s->n;
}

/* One 20 ms block from `t`: the samples there, silence where there are none. */
static void mix_take(mix_src *s, int64_t t, int16_t *out) {
    memset(out, 0, OC_OPUS_FRAME * sizeof *out);
    mix_drop_before(s, t);
    if (s->n == 0) return;
    size_t lead = s->pts_us > t ? (size_t)((s->pts_us - t) * OC_OPUS_RATE / 1000000) : 0;
    if (lead >= OC_OPUS_FRAME) return;
    size_t k = OC_OPUS_FRAME - lead < s->n ? OC_OPUS_FRAME - lead : s->n;
    memcpy(out + lead, s->buf, k * sizeof *out);
    memmove(s->buf, s->buf + k, (s->n - k) * sizeof *s->buf);
    s->n -= k;
    s->pts_us += (int64_t)k * 1000000 / OC_OPUS_RATE;
}

/* Mix the microphone and the computer's sound into 20 ms blocks on one
 * timeline from the first frame. The microphone is echo-cancelled against the
 * computer's sound first -- speakers heard back by the microphone would
 * otherwise be recorded twice, the second time late. A block waits for both
 * sources until it is 300 ms old, then takes silence for what never came: the
 * computer is silent by saying nothing at all. `flush` covers the last frame. */
static void pump_mix(oc_recorder *r, oc_opusenc *oe, enc_ctx *e, int flush) {
    int16_t mic[OC_OPUS_FRAME], loop[OC_OPUS_FRAME], mix[OC_OPUS_FRAME];
    uint8_t pkt[OC_OPUS_MAX_PACKET];
    mix_fill(&r->mic_s);
    mix_fill(&r->loop_s);
    for (;;) {
        int64_t t = r->a_t, now = oc_media_clock_us();
        if (flush) {
            if (t > r->last_video_us) break;
        } else {
            int late = now - t > 300000;
            if (r->mic_s.dev && mix_avail(&r->mic_s, t) < OC_OPUS_FRAME && !late) break;
            if (r->loop_s.dev && mix_avail(&r->loop_s, t) < OC_OPUS_FRAME && !late) break;
            if (now - t < 20000) break;                   /* not yet happened */
        }
        mix_take(&r->mic_s, t, mic);
        mix_take(&r->loop_s, t, loop);
        if (r->mic_s.dev && r->loop_s.dev && r->proc_state)
            r->proc->process(r->proc_state, mic, loop, OC_OPUS_FRAME);
        for (int i = 0; i < OC_OPUS_FRAME; i++) {
            int v = (r->mic_s.dev ? mic[i] : 0) + loop[i];
            mix[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        int n = oc_opusenc_encode(oe, mix, pkt, sizeof pkt);
        if (n < 0 || oc_mp4_write_audio(e->mp4, pkt, (size_t)n, OC_OPUS_FRAME) != 0) { e->failed = 1; return; }
        r->a_t += (int64_t)OC_OPUS_FRAME * 1000000 / OC_OPUS_RATE;
    }
}

static void pump(oc_recorder *r, oc_opusenc *oe, enc_ctx *e, int16_t *acc, size_t *acc_n, int flush) {
    if (r->loop) pump_mix(r, oe, e, flush);
    else if (r->mic) pump_audio(r, oe, e, acc, acc_n, flush);
}

static void *encode_main(void *arg) {
    oc_recorder *r = arg;
    oc_vp9enc  *ve = NULL;
    oc_opusenc *oe = NULL;
    enc_ctx     e = { r, NULL, 0 };
    oc_frame    work = {0}, small = {0};   /* camera size; encode size when they differ */
    int16_t     acc[OC_OPUS_FRAME];
    size_t      acc_n = 0;
    int         recording = 0, enc_fps = 0;
    int64_t     next_frame_us = 0;

    if (oc_frame_alloc(&work, r->width, r->height) != 0) goto fail_early;

    for (;;) {
        oc_mutex_lock(&r->mu);
        while (!r->quit && r->state != OC_REC_RECORDING && r->state != OC_REC_FINISHING)
            oc_cond_wait(&r->cv, &r->mu);
        if (r->quit) { oc_mutex_unlock(&r->mu); break; }

        if (!recording) {
            /* Recording just started. */
            oc_mutex_unlock(&r->mu);
            int ew, eh;
            encode_size(r->width, r->height, r->want_h, &ew, &eh);
            enc_fps = r->fps;
            oc_frame_free(&small);
            if ((ew != r->width || eh != r->height) && oc_frame_alloc(&small, ew, eh) != 0) goto fail;
            ve = r->screen ? oc_vp9enc_open_screen(ew, eh, enc_fps) : oc_vp9enc_open(ew, eh, enc_fps);
            oe = r->mic || r->loop ? oc_opusenc_open() : NULL;
            e.mp4 = oc_mp4_writer_open(ew, eh);
            if (!ve || ((r->mic || r->loop) && !oe) || !e.mp4) goto fail;
            oc_mutex_lock(&r->mu); r->enc_w = ew; r->enc_h = eh; oc_mutex_unlock(&r->mu);
            next_frame_us = 0;
            recording = 1;
            r->t0_us = 0;
            continue;
        }

        int have = r->q_len > 0;
        int finishing = r->stop_requested;
        int64_t behind = 0;
        if (have) {
            oc_frame *slot = &r->pool[r->q_head];
            oc_frame_copy(&work, slot);
            behind = r->pool[(r->q_head + r->q_len - 1) % QUEUE_FRAMES].pts_us - slot->pts_us;
            r->q_head = (r->q_head + 1) % QUEUE_FRAMES;
            r->q_len--;
        }
        if (!have && !finishing) {
            /* Wait briefly for a frame; audio keeps draining meanwhile. */
            oc_mutex_unlock(&r->mu);
            if (r->t0_us) pump(r, oe, &e, acc, &acc_n, 0);
            sleep_ms(5);
            if (e.failed) goto fail;
            continue;
        }
        oc_mutex_unlock(&r->mu);

        if (have) {
            if (!r->t0_us) {
                r->t0_us = work.pts_us;
                r->a_t = work.pts_us;
                /* Drop microphone samples from before the first frame. */
                pump(r, oe, &e, acc, &acc_n, 0);
            }
            uint32_t elapsed = (uint32_t)((work.pts_us - r->t0_us) / 1000);
            if (elapsed >= r->cap_ms || oc_mp4_writer_bytes(e.mp4) >= r->max_bytes) {
                /* The length cap, or the byte budget that keeps the file under
                 * the daemon's video cap — whichever comes first. */
                finishing = 1;
                oc_mutex_lock(&r->mu); r->stop_requested = 1; oc_mutex_unlock(&r->mu);
            } else {
                /* Falling behind: restart the encoder a size smaller, once. The
                 * new encoder's first frame is a keyframe carrying the new size. */
                int sh = step_height(r->enc_h);
                if (!r->stepped_down && behind > BEHIND_US && sh) {
                    oc_vp9enc_encode(ve, NULL, 0, emit_video, &e);
                    oc_vp9enc_close(ve);
                    int ew, eh;
                    encode_size(r->width, r->height, sh, &ew, &eh);
                    enc_fps = sh <= 360 ? 24 : r->fps;
                    ve = r->screen ? oc_vp9enc_open_screen(ew, eh, enc_fps) : oc_vp9enc_open(ew, eh, enc_fps);
                    oc_frame_free(&small);
                    if (!ve || oc_frame_alloc(&small, ew, eh) != 0) goto fail;
                    oc_mutex_lock(&r->mu); r->stepped_down = 1; r->enc_w = ew; r->enc_h = eh; oc_mutex_unlock(&r->mu);
                }
                /* A lower rate than the camera's: skip frames to it. */
                if (work.pts_us >= next_frame_us) {
                    if (enc_fps < r->fps) next_frame_us = work.pts_us + 1000000 / enc_fps - 2000;
                    const oc_frame *in = &work;
                    if (small.plane[0]) { oc_i420_scale(&work, &small); in = &small; }
                    if (oc_vp9enc_encode(ve, in, 0, emit_video, &e) != 0) goto fail;
                }
                oc_mutex_lock(&r->mu); r->elapsed_ms = elapsed; oc_mutex_unlock(&r->mu);
            }
            r->last_video_us = work.pts_us;
            pump(r, oe, &e, acc, &acc_n, 0);
            if (e.failed) goto fail;
        }

        if (finishing) {
            oc_mutex_lock(&r->mu);
            if (r->state == OC_REC_RECORDING) r->state = OC_REC_FINISHING;
            r->q_len = 0;                                   /* frames past the stop are not recorded */
            oc_mutex_unlock(&r->mu);
            if (!r->t0_us) goto fail;                       /* stopped before any frame */
            oc_vp9enc_encode(ve, NULL, 0, emit_video, &e);
            pump(r, oe, &e, acc, &acc_n, 1);
            if (e.failed) goto fail;
            oc_rec_result res = {0};
            int rc = oc_mp4_writer_finish(e.mp4, &res.video, &res.video_len, &res.duration_ms);
            e.mp4 = NULL;
            if (rc != 0) goto fail;
            if (oc_recorder_poster_jpeg(res.video, res.video_len, &res.poster, &res.poster_len,
                                        &res.width, &res.height) != 0) {
                oc_rec_result_free(&res);
                goto fail;
            }
            oc_mutex_lock(&r->mu);
            /* The track's declared size: what recording started at. */
            encode_size(r->width, r->height, r->want_h, &res.width, &res.height);
            oc_mutex_unlock(&r->mu);
            oc_mutex_lock(&r->mu);
            r->result = res;
            r->state = OC_REC_DONE;
            oc_cond_signal(&r->cv);
            oc_mutex_unlock(&r->mu);
            break;
        }
    }
    goto done;

fail:
    oc_mutex_lock(&r->mu);
    r->state = OC_REC_ERROR;
    oc_cond_signal(&r->cv);
    oc_mutex_unlock(&r->mu);
done:
    oc_mp4_writer_abort(e.mp4);
    oc_vp9enc_close(ve);
    oc_opusenc_close(oe);
fail_early:
    oc_frame_free(&work);
    oc_frame_free(&small);
    return NULL;
}

/* ---- public ------------------------------------------------------------------------ */

oc_recorder *oc_recorder_open(const oc_recorder_opts *opts, int *err) {
    int dummy;
    if (!err) err = &dummy;
    oc_recorder_opts o = opts ? *opts : (oc_recorder_opts){0};
    int want_h = o.height > 0 ? o.height : 720;
    int want_w = o.width > 0 ? o.width : (want_h * 16 / 9) & ~1;
    int fps = o.fps > 0 ? o.fps : 30;

    oc_recorder *r = calloc(1, sizeof *r);
    if (!r) { *err = OC_REC_FAILED; return NULL; }
    oc_mutex_init(&r->mu);
    oc_cond_init(&r->cv);
    r->cap_ms = cap_from_env(o.cap_ms ? o.cap_ms : OC_RECORDER_CAP_MS);
    r->fps = fps;
    r->want_h = want_h;
    r->max_bytes = o.max_bytes ? o.max_bytes : OC_RECORDER_MAX_BYTES;

    int cerr;
    r->screen = o.screen_id && *o.screen_id;
    r->corner = o.corner >= OC_CORNER_BR && o.corner <= OC_CORNER_TL ? o.corner : OC_CORNER_BR;
    if (r->screen) {
        r->cam = oc_capture_open_screen(o.screen_id, want_w, want_h, fps, &cerr);
        if (!r->cam) {
            *err = cerr == OC_CAP_DENIED ? OC_REC_SCREEN_DENIED : cerr == OC_CAP_NODEVICE ? OC_REC_SCREEN_UNSUPPORTED
                 : cerr == OC_CAP_GONE ? OC_REC_SCREEN_GONE : OC_REC_FAILED;
            oc_recorder_close(r);
            return NULL;
        }
        if (o.with_camera) {
            /* The box is a fifth of the width: a small camera frame is plenty. */
            r->inset = oc_capture_open(o.camera_id, 640, 360, fps, &cerr);
            if (!r->inset) {
                *err = cerr == OC_CAP_DENIED ? OC_REC_CAMERA_DENIED : cerr == OC_CAP_NODEVICE ? OC_REC_CAMERA_NODEVICE
                     : cerr == OC_CAP_BUSY ? OC_REC_CAMERA_BUSY : OC_REC_FAILED;
                oc_recorder_close(r);
                return NULL;
            }
            if (oc_capture_start(r->inset) != OC_CAP_OK) { *err = OC_REC_FAILED; oc_recorder_close(r); return NULL; }
        }
    } else {
        r->cam = oc_capture_open(o.camera_id, want_w, want_h, fps, &cerr);
        if (!r->cam) {
            *err = cerr == OC_CAP_DENIED ? OC_REC_CAMERA_DENIED : cerr == OC_CAP_NODEVICE ? OC_REC_CAMERA_NODEVICE
                 : cerr == OC_CAP_BUSY ? OC_REC_CAMERA_BUSY : OC_REC_FAILED;
            oc_recorder_close(r);
            return NULL;
        }
    }
    if (oc_capture_start(r->cam) != OC_CAP_OK) { *err = OC_REC_FAILED; oc_recorder_close(r); return NULL; }
    /* The backend reports its real size with the first frame. */
    oc_frame first;
    int rc = 0;
    for (int tries = 0; tries < 30 && rc == 0; tries++) rc = oc_capture_next(r->cam, &first, 100);
    if (rc != 1) {
        if (rc == 0) oc_capture_set_detail(r->screen ? "no frame from the screen within 3 s"
                                                     : "no frame from the camera within 3 s");
        *err = rc == OC_CAP_DENIED ? (r->screen ? OC_REC_SCREEN_DENIED : OC_REC_CAMERA_DENIED)
             : rc == OC_CAP_GONE ? OC_REC_SCREEN_GONE : OC_REC_FAILED;
        oc_recorder_close(r);
        return NULL;
    }
    r->width = first.width; r->height = first.height;
    if (oc_frame_alloc(&r->latest, r->width, r->height) != 0) { *err = OC_REC_FAILED; oc_recorder_close(r); return NULL; }
    if (r->inset && oc_frame_alloc(&r->comp, r->width, r->height) != 0) { *err = OC_REC_FAILED; oc_recorder_close(r); return NULL; }
    for (int i = 0; i < QUEUE_FRAMES; i++)
        if (oc_frame_alloc(&r->pool[i], r->width, r->height) != 0) { *err = OC_REC_FAILED; oc_recorder_close(r); return NULL; }
    oc_frame_copy(&r->latest, &first);
    r->latest_seq = 1;

    int aerr;
    r->mic = oc_audio_capture_open(o.mic_id, OC_OPUS_RATE, 1, &aerr);
    if (!r->mic && aerr == OC_AUDIO_DENIED) { *err = OC_REC_MIC_DENIED; oc_recorder_close(r); return NULL; }
    if (!r->mic && aerr == OC_AUDIO_BUSY) { *err = OC_REC_MIC_BUSY; oc_recorder_close(r); return NULL; }
    /* The computer's sound, when asked for. A machine that cannot give it records
     * without it rather than not at all, as with no microphone. */
    if (r->screen && o.computer_sound) {
        r->loop = oc_audio_loopback_open(NULL, OC_OPUS_RATE, 1, &aerr);
        r->mic_s.dev = r->mic;
        r->loop_s.dev = r->loop;
        if (r->loop && r->mic) {
            r->proc = &OC_PROCESSOR_SPEEX_48K;
            r->proc_state = r->proc->open(OC_OPUS_RATE, OC_OPUS_FRAME);
        }
    }
    r->has_audio = r->mic != NULL || r->loop != NULL;

    r->state = OC_REC_PREVIEW;
    if (r->inset) {
        if (oc_thread_create(&r->inset_thread, inset_main, r) != 0) { *err = OC_REC_FAILED; oc_recorder_close(r); return NULL; }
        r->inset_started = 1;
    }
    if (oc_thread_create(&r->cap_thread, capture_main, r) != 0) { *err = OC_REC_FAILED; oc_recorder_close(r); return NULL; }
    if (oc_thread_create(&r->enc_thread, encode_main, r) != 0) {
        oc_mutex_lock(&r->mu); r->quit = 1; oc_mutex_unlock(&r->mu);
        oc_thread_join(r->cap_thread);
        *err = OC_REC_FAILED;
        oc_recorder_close(r);
        return NULL;
    }
    r->threads_started = 1;
    *err = OC_REC_OK;
    return r;
}

int oc_recorder_preview(oc_recorder *r, uint8_t *bgra, size_t cap, int *w, int *h, uint64_t *seq) {
    oc_mutex_lock(&r->mu);
    *w = r->width; *h = r->height;
    int rc = 0;
    if (r->latest_seq != *seq) {
        if (cap < (size_t)r->width * (size_t)r->height * 4) {
            rc = -1;
        } else {
            oc_i420_to_bgra(&r->latest, bgra, r->width * 4);
            *seq = r->latest_seq;
            rc = 1;
        }
    }
    oc_mutex_unlock(&r->mu);
    return rc;
}

int oc_recorder_start(oc_recorder *r) {
    oc_mutex_lock(&r->mu);
    int ok = r->state == OC_REC_PREVIEW;
    if (ok) {
        r->state = OC_REC_RECORDING;
        r->stop_requested = 0;
        r->q_len = 0;
        r->elapsed_ms = 0;
        oc_cond_signal(&r->cv);
    }
    oc_mutex_unlock(&r->mu);
    return ok ? 0 : -1;
}

void oc_recorder_stop(oc_recorder *r) {
    oc_mutex_lock(&r->mu);
    if (r->state == OC_REC_RECORDING) r->stop_requested = 1;
    oc_cond_signal(&r->cv);
    oc_mutex_unlock(&r->mu);
}

void oc_recorder_status(oc_recorder *r, oc_rec_status *st) {
    oc_mutex_lock(&r->mu);
    st->state = r->state;
    st->elapsed_ms = r->elapsed_ms;
    st->cap_ms = r->cap_ms;
    st->level = r->mic ? oc_audio_level(r->mic) : 0;
    st->has_audio = r->has_audio;
    st->stepped_down = r->stepped_down;
    st->enc_width = r->enc_w;
    st->enc_height = r->enc_h;
    st->dropped_frames = r->dropped;
    st->screen = r->screen;
    st->computer_sound = r->loop != NULL;
    st->source_gone = r->source_gone;
    oc_mutex_unlock(&r->mu);
}

int oc_recorder_take(oc_recorder *r, oc_rec_result *out) {
    oc_mutex_lock(&r->mu);
    int ok = r->state == OC_REC_DONE && r->result.video;
    if (ok) { *out = r->result; memset(&r->result, 0, sizeof r->result); }
    oc_mutex_unlock(&r->mu);
    return ok ? 0 : -1;
}

void oc_recorder_close(oc_recorder *r) {
    if (!r) return;
    if (r->threads_started || r->inset_started) {
        oc_mutex_lock(&r->mu);
        r->quit = 1;
        oc_cond_signal(&r->cv);
        oc_mutex_unlock(&r->mu);
        if (r->threads_started) {
            oc_thread_join(r->cap_thread);
            oc_thread_join(r->enc_thread);
        }
        if (r->inset_started) oc_thread_join(r->inset_thread);
    }
    if (r->cam) { oc_capture_stop(r->cam); oc_capture_close(r->cam); }
    if (r->inset) { oc_capture_stop(r->inset); oc_capture_close(r->inset); }
    oc_audio_close(r->mic);
    oc_audio_close(r->loop);
    if (r->proc_state) r->proc->close(r->proc_state);
    oc_frame_free(&r->inset_latest);
    oc_frame_free(&r->comp);
    oc_frame_free(&r->latest);
    for (int i = 0; i < QUEUE_FRAMES; i++) oc_frame_free(&r->pool[i]);
    oc_rec_result_free(&r->result);
    oc_mutex_destroy(&r->mu);
    oc_cond_destroy(&r->cv);
    free(r);
}

void oc_rec_result_free(oc_rec_result *res) {
    if (!res) return;
    free(res->video); free(res->poster);
    memset(res, 0, sizeof *res);
}

/* ---- the poster ---------------------------------------------------------------------- */

int oc_recorder_poster_index(const oc_mp4_info *info) {
    const oc_mp4_track *v = &info->video;
    int first = -1;
    for (uint32_t i = 0; i < v->n_samples; i++) {
        if (!v->samples[i].sync) continue;
        if (first < 0) first = (int)i;
        if (v->samples[i].dts >= v->timescale) return (int)i;
    }
    return first;
}

typedef struct { uint8_t *d; size_t n, cap; int bad; } jbuf;

static void jpeg_sink(void *ctx, void *data, int size) {
    jbuf *b = ctx;
    if (b->bad || size <= 0) return;
    if (b->n + (size_t)size > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->n + (size_t)size) cap *= 2;
        uint8_t *d = realloc(b->d, cap);
        if (!d) { b->bad = 1; return; }
        b->d = d; b->cap = cap;
    }
    memcpy(b->d + b->n, data, (size_t)size);
    b->n += (size_t)size;
}

int oc_recorder_poster_jpeg(const uint8_t *mp4, size_t len, uint8_t **jpeg, size_t *jpeg_len,
                            int *width, int *height) {
    *jpeg = NULL; *jpeg_len = 0;
    oc_mp4_info info;
    if (oc_mp4_parse(mp4, len, &info) != 0) return -1;
    int idx = oc_recorder_poster_index(&info);
    int rc = -1;
    oc_vp9dec *d = idx >= 0 ? oc_vp9dec_open() : NULL;
    uint8_t *rgb = NULL;
    if (d) {
        const oc_mp4_sample *s = &info.video.samples[idx];
        oc_frame f;
        if (oc_vp9dec_decode(d, mp4 + s->offset, s->size, 0, &f) == 0) {
            size_t px = (size_t)f.width * (size_t)f.height;
            rgb = malloc(px * 4);
            if (rgb) {
                oc_i420_to_bgra(&f, rgb, f.width * 4);
                /* BGRA to RGB in place. */
                for (size_t i = 0; i < px; i++) {
                    uint8_t b = rgb[4 * i], g = rgb[4 * i + 1], rr = rgb[4 * i + 2];
                    rgb[3 * i] = rr; rgb[3 * i + 1] = g; rgb[3 * i + 2] = b;
                }
                jbuf out = {0};
                if (stbi_write_jpg_to_func(jpeg_sink, &out, f.width, f.height, 3, rgb, 82) && !out.bad) {
                    *jpeg = out.d; *jpeg_len = out.n;
                    if (width) *width = f.width;
                    if (height) *height = f.height;
                    rc = 0;
                } else {
                    free(out.d);
                }
            }
        }
    }
    free(rgb);
    oc_vp9dec_close(d);
    oc_mp4_info_free(&info);
    return rc;
}
