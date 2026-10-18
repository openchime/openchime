/* A dictation session (oc_dictate.h). */
#define _POSIX_C_SOURCE 200809L
#include "oc_dictate.h"

#include "audio_dev.h"
#include "oc_processor.h"
#include "oc_segmenter.h"
#include "oc_thread.h"
#include "protocol.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* libfvad's aggressiveness: 2 rejects steady background noise without clipping
 * soft word onsets (VOICE-INPUT.md §4). */
#define DICTATE_VAD_MODE 2

struct oc_dictate {
    oc_client          *client;
    uint8_t             mode;
    uint64_t            channel_id, thread_root;
    oc_audio_dev       *mic;
    const oc_audio_processor *proc;
    void               *proc_state;
    oc_segmenter       *seg;
    oc_thread_t         thread;
    atomic_int          stop;
    atomic_int          send_rest;
    atomic_int          level;
    atomic_int          speaking;
    atomic_uint         sent;
};

static void nap(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static int peak(const int16_t *s, size_t n) {
    int p = 0;
    for (size_t i = 0; i < n; i++) { int v = s[i] < 0 ? -s[i] : s[i]; if (v > p) p = v; }
    return p > 32767 ? 32767 : p;
}

static void send_ready(oc_dictate *d) {
    size_t n = 0;
    int16_t *pcm = oc_seg_take(d->seg, &n);
    if (pcm && n) {
        if (oc_client_stt_send(d->client, d->mode, d->channel_id, d->thread_root, pcm, n))
            atomic_fetch_add(&d->sent, 1);
    }
    free(pcm);                    /* the client copied it; nothing keeps it now */
}

static void *capture_main(void *arg) {
    oc_dictate *d = arg;
    int16_t frame[OC_SEG_FRAME], ref[OC_SEG_FRAME];
    size_t have = 0;
    int64_t frame_pts = 0;
    while (!atomic_load(&d->stop)) {
        int64_t pts = 0;
        size_t got = oc_audio_capture_read(d->mic, frame + have, OC_SEG_FRAME - have, &pts);
        if (got == 0) { nap(5); continue; }
        if (have == 0) frame_pts = pts;
        have += got;
        if (have < OC_SEG_FRAME) continue;
        have = 0;
        /* The client's own sound out first: what the speaker played at this
         * moment is subtracted from what the microphone heard. */
        oc_audio_reference(frame_pts, ref, OC_SEG_FRAME);
        d->proc->process(d->proc_state, frame, ref, OC_SEG_FRAME);
        atomic_store(&d->level, peak(frame, OC_SEG_FRAME));
        if (oc_seg_push(d->seg, frame)) send_ready(d);
        atomic_store(&d->speaking, oc_seg_speaking(d->seg));
    }
    if (atomic_load(&d->send_rest) && oc_seg_finish(d->seg)) send_ready(d);
    return NULL;
}

oc_dictate *oc_dictate_start(oc_client *c, uint8_t mode, uint64_t channel_id, uint64_t thread_root,
                             const char *mic_id, uint32_t max_ms, int *err) {
    int dummy;
    if (!err) err = &dummy;
    if (!c || (mode != OC_STT_MODE_PTT && mode != OC_STT_MODE_FREE) ||
        (mode == OC_STT_MODE_FREE && channel_id == 0)) { *err = OC_DICTATE_FAILED; return NULL; }
    oc_dictate *d = calloc(1, sizeof *d);
    if (!d) { *err = OC_DICTATE_FAILED; return NULL; }
    d->client = c;
    d->mode = mode;
    d->channel_id = channel_id;
    d->thread_root = thread_root;
    uint32_t cap_ms = max_ms ? max_ms : 30000;
    d->seg = oc_seg_new(mode == OC_STT_MODE_PTT, (size_t)cap_ms * (OC_SEG_RATE / 1000), DICTATE_VAD_MODE);
    d->proc = &OC_PROCESSOR_SPEEX;
    d->proc_state = d->proc->open(OC_SEG_RATE, OC_SEG_FRAME);
    if (!d->proc_state) { d->proc = &OC_PROCESSOR_NONE; d->proc_state = d->proc->open(OC_SEG_RATE, OC_SEG_FRAME); }
    if (!d->seg) { oc_dictate_stop(d, 0); *err = OC_DICTATE_FAILED; return NULL; }
    int aerr = 0;
    d->mic = oc_audio_capture_open(mic_id, OC_SEG_RATE, 1, &aerr);
    if (!d->mic) {
        oc_dictate_stop(d, 0);
        *err = aerr == OC_AUDIO_DENIED ? OC_DICTATE_DENIED : aerr == OC_AUDIO_NODEVICE ? OC_DICTATE_NODEVICE
             : aerr == OC_AUDIO_BUSY ? OC_DICTATE_BUSY : OC_DICTATE_FAILED;
        return NULL;
    }
    if (oc_thread_create(&d->thread, capture_main, d) != 0) {
        oc_audio_close(d->mic);
        d->mic = NULL;
        oc_dictate_stop(d, 0);
        *err = OC_DICTATE_FAILED;
        return NULL;
    }
    *err = OC_DICTATE_OK;
    return d;
}

void oc_dictate_stop(oc_dictate *d, int send_rest) {
    if (!d) return;
    if (d->mic) {
        atomic_store(&d->send_rest, send_rest ? 1 : 0);
        atomic_store(&d->stop, 1);
        oc_thread_join(d->thread);
        oc_audio_close(d->mic);          /* released the moment the session ends */
    }
    if (d->proc_state) d->proc->close(d->proc_state);
    oc_seg_free(d->seg);
    free(d);
}

uint8_t  oc_dictate_mode(const oc_dictate *d)        { return d->mode; }
uint64_t oc_dictate_channel(const oc_dictate *d)     { return d->channel_id; }
uint64_t oc_dictate_thread_root(const oc_dictate *d) { return d->thread_root; }
int      oc_dictate_level(const oc_dictate *d)       { return atomic_load(&((oc_dictate *)d)->level); }
int      oc_dictate_speaking(const oc_dictate *d)    { return atomic_load(&((oc_dictate *)d)->speaking); }
uint32_t oc_dictate_sent(const oc_dictate *d)        { return atomic_load(&((oc_dictate *)d)->sent); }
