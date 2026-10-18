/* The jitter buffer — see oc_jitter.h. */
#include "oc_jitter.h"

#include <stdlib.h>
#include <string.h>

void oc_jb_init(oc_jitter *j) {
    memset(j, 0, sizeof *j);
    j->target = OC_JB_MIN_MS / OC_JB_FRAME_MS;
}

int oc_jb_target_ms(const oc_jitter *j) { return j->target * OC_JB_FRAME_MS; }

static int cmp64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* The target from the recent arrivals: how much later than the earliest the
 * slowest 5% came, plus a frame. */
static void retarget(oc_jitter *j) {
    int64_t v[OC_JB_HIST];
    int n = j->n_delay;
    if (n < 2) return;
    memcpy(v, j->delay, (size_t)n * sizeof v[0]);
    qsort(v, (size_t)n, sizeof v[0], cmp64);
    int64_t spread = v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1] - v[0];
    int ms = (int)spread + OC_JB_FRAME_MS;
    if (ms < OC_JB_MIN_MS) ms = OC_JB_MIN_MS;
    if (ms > OC_JB_MAX_MS) ms = OC_JB_MAX_MS;
    j->target = (ms + OC_JB_FRAME_MS - 1) / OC_JB_FRAME_MS;
}

int oc_jb_put(oc_jitter *j, uint32_t frame, const uint8_t *data, size_t len, int64_t now_ms) {
    if (len > OC_JB_MAX_PACKET) return 2;
    j->delay[j->i_delay] = now_ms - (int64_t)frame * OC_JB_FRAME_MS;
    j->i_delay = (j->i_delay + 1) % OC_JB_HIST;
    if (j->n_delay < OC_JB_HIST) j->n_delay++;
    retarget(j);

    if (!j->started) {
        /* The first packet: play it once the target's worth has had time to
         * arrive behind it. */
        j->started = 1;
        j->play = frame;
        j->newest = frame;
        j->grow = j->target;
    } else if (j->idle && (int32_t)(frame - j->newest) > 0) {
        /* A new talk spurt after silence: start it at the target's delay rather
         * than wherever the silence left the clock. Only a frame newer than
         * everything seen starts one; a straggler from the last spurt does not. */
        j->play = frame;
        j->grow = j->target;
        j->idle = 0;
    }
    if ((int32_t)(frame - j->play) < 0) { j->late++; if (j->grow < 1) j->grow = 1; return 2; }
    if ((int32_t)(frame - j->play) >= OC_JB_SLOTS) {
        /* Far ahead of what is playing: a clock jump. Start again from here. */
        memset(j->slot, 0, sizeof j->slot);
        j->play = frame;
        j->newest = frame;
        j->grow = j->target;
    }
    oc_jb_slot *s = &j->slot[frame % OC_JB_SLOTS];
    if (s->used && s->frame == frame) { j->dup++; return 1; }
    s->used = 1;
    s->frame = frame;
    s->len = (uint16_t)len;
    if (len) memcpy(s->data, data, len);
    if ((int32_t)(frame - j->newest) > 0) j->newest = frame;
    j->received++;
    return 0;
}

static oc_jb_slot *at(oc_jitter *j, uint32_t frame) {
    oc_jb_slot *s = &j->slot[frame % OC_JB_SLOTS];
    return s->used && s->frame == frame ? s : NULL;
}

int oc_jb_get(oc_jitter *j, int quiet, const uint8_t **data, size_t *len) {
    *data = NULL;
    *len = 0;
    if (!j->started) return OC_JB_NONE;
    if (j->grow > 0) {
        /* Waiting to reach the target. Mid-speech that is a concealed frame, so
         * the voice stretches rather than stops; before a talk spurt it is
         * nothing at all. */
        j->grow--;
        if (j->concealed > 0 && j->concealed < OC_JB_PLC_MAX) { j->concealed++; j->plc++; return OC_JB_PLC; }
        return OC_JB_NONE;
    }
    /* More buffered than the target wants, and nobody is speaking: drop a frame
     * to give the delay back. */
    int32_t depth = (int32_t)(j->newest - j->play);
    if (quiet && depth > j->target + 1) {
        oc_jb_slot *s = at(j, j->play);
        if (s) s->used = 0;
        j->play++;
        j->skipped++;
    }
    uint32_t f = j->play++;
    oc_jb_slot *s = at(j, f);
    if (s) {
        s->used = 0;
        j->concealed = 0;
        j->idle = 0;
        *data = s->data;
        *len = s->len;
        return OC_JB_PACKET;
    }
    oc_jb_slot *next = at(j, f + 1);
    if (next && next->len) {
        j->fec++;
        j->concealed = 0;
        *data = next->data;
        *len = next->len;
        return OC_JB_FEC;
    }
    if ((int32_t)(f - j->newest) > 0) {
        /* Past everything that has arrived: the sender stopped (DTX), or went.
         * A few frames of concealment smooth the stop; then nothing. */
        if (j->concealed < OC_JB_PLC_MAX && j->concealed >= 0 && !j->idle) {
            j->concealed++;
            j->plc++;
            if (j->concealed == OC_JB_PLC_MAX) j->idle = 1;
            return OC_JB_PLC;
        }
        j->idle = 1;
        j->concealed = 0;
        return OC_JB_NONE;
    }
    /* A hole in the middle of what arrived: lost. */
    if (j->concealed < OC_JB_PLC_MAX) { j->concealed++; j->plc++; return OC_JB_PLC; }
    return OC_JB_NONE;
}
