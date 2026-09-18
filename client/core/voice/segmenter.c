/* Where an utterance ends (oc_segmenter.h). */
#include "oc_segmenter.h"

#include <fvad.h>

#include <stdlib.h>
#include <string.h>

struct oc_segmenter {
    Fvad    *vad;
    int      held;
    size_t   max;               /* the cap, in samples */
    int16_t *buf;               /* the segment being built */
    size_t   len;
    int      open;              /* free talk: a segment has begun */
    /* Free talk before onset: the pre-roll, as a ring of frames. */
    int16_t  pre[OC_SEG_PREROLL_FRAMES][OC_SEG_FRAME];
    int      pre_n, pre_at;
    int      onset_run;
    int      quiet_run;         /* frames of non-speech since the last speech */
    size_t   speech;            /* speech frames in the segment */
    size_t   cut;               /* the last place a pause makes a clean cut, 0 = none */
    int      speaking;
    int      frames_seen;       /* for the detector's warm-up */
    int16_t *ready;
    size_t   ready_len;
};

oc_segmenter *oc_seg_new(int held, size_t max_samples, int vad_mode) {
    oc_segmenter *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->held = held;
    s->max = max_samples < OC_SEG_FRAME * 10 ? OC_SEG_FRAME * 10 : max_samples;
    s->buf = malloc((s->max + OC_SEG_FRAME) * sizeof *s->buf);
    s->vad = fvad_new();
    if (!s->buf || !s->vad || fvad_set_sample_rate(s->vad, OC_SEG_RATE) != 0 ||
        fvad_set_mode(s->vad, vad_mode < 0 ? 0 : vad_mode > 3 ? 3 : vad_mode) != 0) {
        oc_seg_free(s);
        return NULL;
    }
    return s;
}

void oc_seg_free(oc_segmenter *s) {
    if (!s) return;
    if (s->vad) fvad_free(s->vad);
    free(s->buf);
    free(s->ready);
    free(s);
}

/* Hand out [0, n) of the buffer as the ready segment and keep the rest. */
static void emit(oc_segmenter *s, size_t n) {
    free(s->ready);
    s->ready = malloc((n ? n : 1) * sizeof *s->ready);
    if (s->ready) memcpy(s->ready, s->buf, n * sizeof *s->ready);
    s->ready_len = s->ready ? n : 0;
    memmove(s->buf, s->buf + n, (s->len - n) * sizeof *s->buf);
    s->len -= n;
    s->cut = 0;
    s->speech = s->len ? 1 : 0;   /* what carries over was mid-speech */
}

static void reset(oc_segmenter *s) {
    s->len = 0;
    s->open = 0;
    s->speech = 0;
    s->cut = 0;
    s->quiet_run = 0;
    s->onset_run = 0;
    s->pre_n = 0;
    s->pre_at = 0;
}

static void append(oc_segmenter *s, const int16_t *frame) {
    memcpy(s->buf + s->len, frame, OC_SEG_FRAME * sizeof *frame);
    s->len += OC_SEG_FRAME;
}

/* At the cap: cut at the last pause, or at the cap if there was none. */
static int cap_check(oc_segmenter *s) {
    if (s->len < s->max) return 0;
    emit(s, s->cut ? s->cut : s->max);
    return s->ready != NULL;
}

int oc_seg_push(oc_segmenter *s, const int16_t *frame) {
    int v = fvad_process(s->vad, frame, OC_SEG_FRAME);
    /* The detector adapts to the room over its first frames and calls them
     * speech meanwhile; believing it would open a segment on the silence
     * before anyone spoke. */
    int speech = v == 1 && s->frames_seen >= OC_SEG_WARMUP_FRAMES;
    if (s->frames_seen < OC_SEG_WARMUP_FRAMES) s->frames_seen++;
    s->speaking = speech;

    if (s->held) {
        append(s, frame);
        if (speech) {
            s->speech++;
            s->quiet_run = 0;
        } else if (++s->quiet_run == OC_SEG_PAUSE_FRAMES && s->speech) {
            s->cut = s->len;
        }
        return cap_check(s);
    }

    if (!s->open) {
        /* Keep the pre-roll; open on a run of speech. */
        memcpy(s->pre[s->pre_at], frame, sizeof s->pre[0]);
        s->pre_at = (s->pre_at + 1) % OC_SEG_PREROLL_FRAMES;
        if (s->pre_n < OC_SEG_PREROLL_FRAMES) s->pre_n++;
        s->onset_run = speech ? s->onset_run + 1 : 0;
        if (s->onset_run < OC_SEG_ONSET_FRAMES) return 0;
        s->open = 1;
        s->len = 0;
        int first = (s->pre_at - s->pre_n + OC_SEG_PREROLL_FRAMES) % OC_SEG_PREROLL_FRAMES;
        for (int i = 0; i < s->pre_n; i++) append(s, s->pre[(first + i) % OC_SEG_PREROLL_FRAMES]);
        s->speech = (size_t)s->onset_run;
        s->quiet_run = 0;
        s->cut = 0;
        return 0;
    }

    append(s, frame);
    if (speech) {
        s->speech++;
        s->quiet_run = 0;
    } else {
        s->quiet_run++;
        if (s->quiet_run == OC_SEG_PAUSE_FRAMES) s->cut = s->len;
        if (s->quiet_run >= OC_SEG_END_FRAMES) {
            /* The utterance is over: keep a little of the quiet, drop the rest. */
            size_t keep = s->len - (size_t)(s->quiet_run - OC_SEG_TAIL_FRAMES) * OC_SEG_FRAME;
            int enough = s->speech >= OC_SEG_MIN_SPEECH;
            if (enough) { s->len = keep; emit(s, keep); }
            reset(s);
            return enough && s->ready;
        }
    }
    return cap_check(s);
}

int oc_seg_finish(oc_segmenter *s) {
    int enough = s->len && s->speech >= (s->held ? OC_SEG_MIN_SPEECH_HELD : OC_SEG_MIN_SPEECH);
    if (enough) emit(s, s->len);
    reset(s);
    return enough && s->ready;
}

int16_t *oc_seg_take(oc_segmenter *s, size_t *samples) {
    int16_t *r = s->ready;
    *samples = r ? s->ready_len : 0;
    s->ready = NULL;
    s->ready_len = 0;
    return r;
}

int oc_seg_speaking(const oc_segmenter *s) { return s->speaking; }
