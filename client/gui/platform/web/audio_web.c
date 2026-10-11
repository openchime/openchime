/* OpenChime web client -- the audio backend over Web Audio (audio_backend.h,
 * WEB.md). A device is an AudioWorklet node whose process callback runs this
 * code on the browser's audio rendering thread, as miniaudio's callbacks run
 * on the device's: it converts the browser's planar floats and hands the
 * frames to the device layer (oc_audio_dev_capture_in /
 * oc_audio_dev_playback_out), which keeps the rings every media thread reads
 * and writes, the timestamps and the far-end reference, all per render
 * quantum. One AudioContext per sample rate, made and started on the page's
 * thread (Emscripten's worklet support, emscripten/webaudio.h). A microphone
 * is a getUserMedia stream into such a node; a speaker is such a node into
 * the destination. The computer's own sound (loopback) is the audio track the
 * browser gives with a screen capture (cap_web.c): a tab's sound anywhere,
 * the system's on Chromium for Windows; where it gives none, the device is
 * not there.
 *
 * Where the browser cannot run the worklet (no audioWorklet, or the context
 * refused to start it), the ScriptProcessorNode path below stands in: the
 * same conversion in JavaScript on the page's thread, with a buffer of 1024
 * frames, which is the latency it costs. The console says which ran.
 *
 * Opening a microphone waits for the permission prompt; the waiting is
 * Asyncify's on the page's thread and a sleep on any other. */
#include "audio_backend.h"
#include "oc_media.h"
#include "devices_web.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/webaudio.h>
#include <speex/speex_resampler.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Run `fn` on the page's thread and wait for it (its own thread: just call). */
static void on_main(void (*fn)(void *), void *arg) {
    if (emscripten_is_main_runtime_thread()) { fn(arg); return; }
    emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), fn, arg);
}

static void wait_ms(int ms) {
    if (emscripten_is_main_runtime_thread()) { emscripten_sleep((unsigned)ms); return; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---- the contexts: one per rate, with its worklet thread --------------------- */

#define WORKLET_STACK (64 * 1024)
enum { CTX_NONE = 0, CTX_STARTING, CTX_READY, CTX_FAILED };

typedef struct {
    int                  rate;
    EMSCRIPTEN_WEBAUDIO_T ctx;
    atomic_int           state;
    void                *stack;
} wctx;
static wctx g_ctx[4];

static void processor_created(EMSCRIPTEN_WEBAUDIO_T ctx, bool ok, void *ud) {
    (void)ctx;
    wctx *c = ud;
    atomic_store(&c->state, ok ? CTX_READY : CTX_FAILED);
    printf("oc-web: audio %s\n", ok ? "worklet" : "scriptprocessor (the worklet's processor did not load)");
}

static void worklet_started(EMSCRIPTEN_WEBAUDIO_T ctx, bool ok, void *ud) {
    wctx *c = ud;
    if (!ok) { atomic_store(&c->state, CTX_FAILED); printf("oc-web: audio scriptprocessor (no worklet thread)\n"); return; }
    WebAudioWorkletProcessorCreateOptions opts = { .name = "oc-dev", .numAudioParams = 0, .audioParamDescriptors = NULL };
    emscripten_create_wasm_audio_worklet_processor_async(ctx, &opts, processor_created, c);
}

/* The context's JS object, kept for the fallback and resumed on the next
 * gesture if the autoplay policy left it suspended. */
EM_JS(void, ctx_js_register, (int handle, int rate), {
    const reg = Module.ocAud || (Module.ocAud = { ctx: {}, d: {} });
    const ctx = emscriptenGetAudioObject(handle);
    reg.ctx[rate] = ctx;
    const resume = () => { try { ctx.resume(); } catch (e) {} };
    document.addEventListener("pointerdown", resume, { once: true });
    document.addEventListener("keydown", resume, { once: true });
    resume();
});

struct ctx_args { int rate; wctx *c; };
static void ctx_start_main(void *arg) {
    struct ctx_args *a = arg;
    wctx *c = a->c;
    EmscriptenWebAudioCreateAttributes attrs = { .latencyHint = "interactive", .sampleRate = (uint32_t)a->rate,
                                                 .renderSizeHint = AUDIO_CONTEXT_RENDER_SIZE_DEFAULT };
    c->rate = a->rate;
    c->ctx = emscripten_create_audio_context(&attrs);
    if (!c->ctx) { atomic_store(&c->state, CTX_FAILED); return; }
    ctx_js_register(c->ctx, a->rate);
    c->stack = aligned_alloc(16, WORKLET_STACK);
    if (!c->stack) { atomic_store(&c->state, CTX_FAILED); return; }
    atomic_store(&c->state, CTX_STARTING);
    emscripten_start_wasm_audio_worklet_thread_async(c->ctx, c->stack, WORKLET_STACK, worklet_started, c);
}

/* The context for `rate`, started if it is not yet; waits for the worklet
 * to be up or to have failed (then the fallback is used). */
static wctx *ctx_for(int rate) {
    wctx *c = NULL;
    for (size_t i = 0; i < sizeof g_ctx / sizeof g_ctx[0] && !c; i++)
        if (g_ctx[i].rate == rate || g_ctx[i].rate == 0) c = &g_ctx[i];
    if (!c) return NULL;
    if (c->rate == 0 || atomic_load(&c->state) == CTX_NONE) {
        struct ctx_args a = { rate, c };
        on_main(ctx_start_main, &a);
    }
    for (int waited = 0; atomic_load(&c->state) == CTX_STARTING && waited < 15000; waited += 20) wait_ms(20);
    if (atomic_load(&c->state) == CTX_STARTING) atomic_store(&c->state, CTX_FAILED);
    return c;
}

/* ---- a device -------------------------------------------------------------------- */

typedef struct {
    int           handle;
    oc_audio_dev *dev;
    int           capture, channels, dev_rate, ctx_rate;
    int           worklet;             /* 1 on the worklet, 0 on the ScriptProcessor fallback */
    EMSCRIPTEN_WEBAUDIO_T node;
    /* The callback's scratch, sized for one quantum (or the fallback's buffer). */
    int16_t      *buf;
    size_t        buf_frames;
    /* When the browser gave the context another rate than the device's (Safari
     * for a microphone): the device's rate on this side of a resampler. */
    SpeexResamplerState *rs;
    int16_t      *rs_buf;              /* device-rate frames, one quantum's worth and some */
    size_t        rs_cap;
    float        *fifo;                /* playback: context-rate frames resampled ahead */
    size_t        fifo_n, fifo_cap;
    int           state;               /* written by the page's thread: 0 opening, 1 open, <0 OC_AUDIO_* */
} wdev;

static inline int16_t f2i(float v) { return v >= 1.0f ? 32767 : v <= -1.0f ? -32768 : (int16_t)(v * 32767.0f); }

/* The worklet's process callback: on the audio rendering thread, per quantum.
 * No allocation, no lock, no I/O: the device layer's functions are the
 * lock-free rings. */
static bool process_cb(int n_in, const AudioSampleFrame *in, int n_out, AudioSampleFrame *out,
                       int n_params, const AudioParamFrame *params, void *ud) {
    (void)n_params; (void)params;
    wdev *w = ud;
    int ch = w->channels;
    if (w->capture) {
        if (n_in < 1 || in[0].numberOfChannels < 1) return true;     /* nothing connected yet */
        int frames = in[0].samplesPerChannel, have = in[0].numberOfChannels;
        if ((size_t)frames > w->buf_frames) frames = (int)w->buf_frames;
        for (int i = 0; i < frames; i++)
            for (int c = 0; c < ch; c++) {
                int src = c < have ? c : have - 1;
                w->buf[i * ch + c] = f2i(in[0].data[src * in[0].samplesPerChannel + i]);
            }
        if (!w->rs) { oc_audio_dev_capture_in(w->dev, w->buf, (size_t)frames); }
        else {
            spx_uint32_t il = (spx_uint32_t)frames, ol = (spx_uint32_t)w->rs_cap;
            speex_resampler_process_interleaved_int(w->rs, w->buf, &il, w->rs_buf, &ol);
            if (ol) oc_audio_dev_capture_in(w->dev, w->rs_buf, ol);
        }
        /* The silent output that keeps the node scheduled. */
        for (int o = 0; o < n_out; o++) memset(out[o].data, 0, sizeof(float) * (size_t)out[o].numberOfChannels * (size_t)out[o].samplesPerChannel);
        return true;
    }
    if (n_out < 1) return true;
    int frames = out[0].samplesPerChannel, och = out[0].numberOfChannels;
    if (!w->rs) {
        if ((size_t)frames > w->buf_frames) frames = (int)w->buf_frames;
        oc_audio_dev_playback_out(w->dev, w->buf, (size_t)frames);
        for (int c = 0; c < och; c++)
            for (int i = 0; i < frames; i++) out[0].data[c * out[0].samplesPerChannel + i] = w->buf[i * ch + (c < ch ? c : ch - 1)] / 32768.0f;
        return true;
    }
    /* Resampling: pull device-rate frames until the fifo holds a quantum of
     * context-rate frames, then hand that out. */
    while (w->fifo_n < (size_t)frames) {
        size_t want = w->rs_cap;
        oc_audio_dev_playback_out(w->dev, w->rs_buf, want);
        spx_uint32_t il = (spx_uint32_t)want, ol = (spx_uint32_t)((w->fifo_cap - w->fifo_n) / (size_t)ch);
        if (!ol) break;
        speex_resampler_process_interleaved_int(w->rs, w->rs_buf, &il, w->buf, &ol);
        for (size_t k = 0; k < (size_t)ol * (size_t)ch; k++) w->fifo[w->fifo_n * (size_t)ch + k] = w->buf[k] / 32768.0f;
        w->fifo_n += ol;
        if (!il) break;
    }
    size_t give = w->fifo_n < (size_t)frames ? w->fifo_n : (size_t)frames;
    for (int c = 0; c < och; c++)
        for (int i = 0; i < frames; i++)
            out[0].data[c * out[0].samplesPerChannel + i] = (size_t)i < give ? w->fifo[(size_t)i * (size_t)ch + (size_t)(c < ch ? c : ch - 1)] : 0.0f;
    memmove(w->fifo, w->fifo + give * (size_t)ch, (w->fifo_n - give) * (size_t)ch * sizeof *w->fifo);
    w->fifo_n -= give;
    return true;
}

/* The microphone (or the screen's audio track) into the worklet node, on the
 * page's thread; the state is written when the stream is there. */
EM_JS(void, aud_js_source, (int handle, int node, int loopback, const char *id, int channels, int *state), {
    const reg = Module.ocAud || (Module.ocAud = { ctx: {}, d: {} });
    const devid = UTF8ToString(id);
    const fail = (e) => {
        const n = e && e.name;
        HEAP32[state >> 2] = n === "NotAllowedError" || n === "SecurityError" ? -1
                           : n === "NotFoundError" || n === "OverconstrainedError" ? -2 : -3;
        console.warn("audio: " + (e && e.message ? e.message : e));
    };
    (async () => {
        try {
            const worklet = emscriptenGetAudioObject(node);
            const ctx = worklet.context;
            if (ctx.state !== "running") { try { await ctx.resume(); } catch (e) {} }
            const d = { ctx, nodes: [] };
            if (loopback) {
                const caps = Module.ocCap ? Object.values(Module.ocCap.s) : [];
                const scr = caps.filter((c) => c.screen && c.stream.getAudioTracks().length > 0).sort((x, y) => y.opened - x.opened)[0];
                if (!scr) throw { name: "NotFoundError", message: "the screen capture has no audio" };
                d.stream = new MediaStream([scr.stream.getAudioTracks()[0]]);
                d.borrowed = true;
            } else {
                const a = { channelCount: channels, echoCancellation: true, noiseSuppression: false, autoGainControl: false };
                if (devid && devid !== "default") a.deviceId = { exact: devid };
                d.stream = await navigator.mediaDevices.getUserMedia({ audio: a, video: false });
            }
            const src = ctx.createMediaStreamSource(d.stream);
            src.connect(worklet);
            d.nodes.push(src);
            reg.d[handle] = d;
            HEAP32[state >> 2] = 1;
        } catch (e) { fail(e); }
    })();
});

/* ---- the fallback: a ScriptProcessorNode on the page's thread --------------------- */

EMSCRIPTEN_KEEPALIVE void oc_web_audio_in(oc_audio_dev *d, const int16_t *pcm, int frames) {
    oc_audio_dev_capture_in(d, pcm, (size_t)frames);
}
EMSCRIPTEN_KEEPALIVE void oc_web_audio_out(oc_audio_dev *d, int16_t *pcm, int frames) {
    oc_audio_dev_playback_out(d, pcm, (size_t)frames);
}

EM_JS(void, aud_js_open_sp, (int handle, oc_audio_dev *dev, int capture, int loopback, const char *id, int rate, int channels,
                             int16_t *buf, int buf_frames, int *state), {
    const reg = Module.ocAud || (Module.ocAud = { ctx: {}, d: {} });
    const devid = UTF8ToString(id);
    const fail = (e) => {
        const n = e && e.name;
        HEAP32[state >> 2] = n === "NotAllowedError" || n === "SecurityError" ? -1
                           : n === "NotFoundError" || n === "OverconstrainedError" ? -2 : -3;
        console.warn("audio: " + (e && e.message ? e.message : e));
    };
    (async () => {
        try {
            let ctx = reg.ctx[rate];
            if (!ctx) {
                ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: rate });
                reg.ctx[rate] = ctx;
                const resume = () => { ctx.resume(); };
                document.addEventListener("pointerdown", resume, { once: true });
                document.addEventListener("keydown", resume, { once: true });
            }
            if (ctx.state !== "running") { try { await ctx.resume(); } catch (e) {} }
            const d = { ctx, nodes: [] };
            if (capture) {
                if (loopback) {
                    const caps = Module.ocCap ? Object.values(Module.ocCap.s) : [];
                    const scr = caps.filter((c) => c.screen && c.stream.getAudioTracks().length > 0).sort((x, y) => y.opened - x.opened)[0];
                    if (!scr) throw { name: "NotFoundError", message: "the screen capture has no audio" };
                    d.stream = new MediaStream([scr.stream.getAudioTracks()[0]]);
                    d.borrowed = true;
                } else {
                    const a = { channelCount: channels, echoCancellation: true, noiseSuppression: false, autoGainControl: false };
                    if (devid && devid !== "default") a.deviceId = { exact: devid };
                    d.stream = await navigator.mediaDevices.getUserMedia({ audio: a, video: false });
                }
                const src = ctx.createMediaStreamSource(d.stream);
                const node = ctx.createScriptProcessor(buf_frames, channels, 1);
                const sink = ctx.createGain(); sink.gain.value = 0;
                node.onaudioprocess = (ev) => {
                    const inb = ev.inputBuffer, n = inb.length, ch = Math.min(channels, inb.numberOfChannels);
                    const base = buf >> 1;
                    for (let c = 0; c < ch; c++) {
                        const data = inb.getChannelData(c);
                        for (let i = 0; i < n; i++) {
                            const v = data[i];
                            HEAP16[base + i * channels + c] = v >= 1 ? 32767 : v <= -1 ? -32768 : (v * 32767) | 0;
                        }
                    }
                    Module._oc_web_audio_in(dev, buf, n);
                };
                src.connect(node); node.connect(sink); sink.connect(ctx.destination);
                d.nodes.push(src, node, sink);
            } else {
                const node = ctx.createScriptProcessor(buf_frames, 1, channels);
                node.onaudioprocess = (ev) => {
                    const outb = ev.outputBuffer, n = outb.length;
                    Module._oc_web_audio_out(dev, buf, n);
                    const base = buf >> 1;
                    for (let c = 0; c < channels; c++) {
                        const data = outb.getChannelData(c);
                        for (let i = 0; i < n; i++) data[i] = HEAP16[base + i * channels + c] / 32768;
                    }
                };
                const silence = ctx.createConstantSource ? ctx.createConstantSource() : null;
                if (silence) { silence.offset.value = 0; silence.connect(node); silence.start(); d.nodes.push(silence); }
                node.connect(ctx.destination);
                d.nodes.push(node);
            }
            reg.d[handle] = d;
            HEAP32[state >> 2] = 1;
        } catch (e) { fail(e); }
    })();
});

EM_JS(void, aud_js_close, (int handle), {
    const reg = Module.ocAud; const d = reg && reg.d[handle];
    if (!d) return;
    delete reg.d[handle];
    try { for (const n of d.nodes) { if (n.onaudioprocess) n.onaudioprocess = null; n.disconnect(); } } catch (e) {}
    try { if (d.stream && !d.borrowed) d.stream.getTracks().forEach((t) => t.stop()); } catch (e) {}
});

/* ---- the backend ------------------------------------------------------------------- */

static int web_list(int capture, oc_audio_device *out, int cap) {
    return oc_webdev_list(capture ? "audioinput" : "audiooutput", out->id, cap, (int)sizeof *out,
                          (int)sizeof out->id, (int)offsetof(oc_audio_device, name), (int)sizeof out->name,
                          (int)offsetof(oc_audio_device, is_default));
}

static void close_main(void *arg) {
    wdev *w = arg;
    aud_js_close(w->handle);
    if (w->node) { emscripten_destroy_web_audio_node(w->node); w->node = 0; }
}

static void web_close(oc_audio_dev *d) {
    wdev *w = *oc_audio_dev_impl(d);
    if (!w) return;
    on_main(close_main, w);
    if (w->rs) speex_resampler_destroy(w->rs);
    free(w->rs_buf); free(w->fifo); free(w->buf);
    free(w);
    *oc_audio_dev_impl(d) = NULL;
}

struct open_args { wdev *w; wctx *c; int loopback; const char *id; };

/* The worklet route, on the page's thread: the node, its connections. */
static void open_worklet_main(void *arg) {
    struct open_args *o = arg;
    wdev *w = o->w;
    int counts[1] = { w->channels };
    EmscriptenAudioWorkletNodeCreateOptions opts = {
        .numberOfInputs = w->capture ? 1 : 0,
        .numberOfOutputs = 1,
        .outputChannelCounts = w->capture ? (int[]){ 1 } : counts,
        .channelCount = (unsigned long)w->channels,
        .channelCountMode = WEBAUDIO_CHANNEL_COUNT_MODE_EXPLICIT,
        .channelInterpretation = WEBAUDIO_CHANNEL_INTERPRETATION_SPEAKERS,
    };
    w->node = emscripten_create_wasm_audio_worklet_node(o->c->ctx, "oc-dev", &opts, process_cb, w);
    if (!w->node) { w->state = OC_AUDIO_FAILED; return; }
    /* To the destination either way: a capture node's silent output keeps it
     * scheduled; a playback node is heard. */
    emscripten_audio_node_connect(w->node, o->c->ctx, 0, 0);
    if (w->capture) aud_js_source(w->handle, w->node, o->loopback, o->id, w->channels, &w->state);
    else { emscripten_resume_audio_context_async(o->c->ctx, NULL, NULL); w->state = 1; }
}

static void open_sp_main(void *arg) {
    struct open_args *o = arg;
    wdev *w = o->w;
    aud_js_open_sp(w->handle, w->dev, w->capture, o->loopback, o->id, w->dev_rate, w->channels,
                   w->buf, (int)w->buf_frames, &w->state);
}

static int web_open(oc_audio_dev *d, int capture, int loopback, const char *id) {
    static int next_handle = 1;
    wdev *w = calloc(1, sizeof *w);
    if (!w) return OC_AUDIO_FAILED;
    w->handle = next_handle++;
    w->dev = d;
    w->capture = capture;
    w->channels = oc_audio_dev_channels(d);
    w->dev_rate = oc_audio_dev_rate(d);
    *oc_audio_dev_impl(d) = w;

    wctx *c = ctx_for(w->dev_rate);
    w->worklet = c && atomic_load(&c->state) == CTX_READY;
    if (w->worklet) {
        w->ctx_rate = emscripten_audio_context_sample_rate(c->ctx);
        int q = emscripten_audio_context_quantum_size(c->ctx);
        w->buf_frames = (size_t)(q > 0 ? q : 128) * 4;
        w->buf = calloc(w->buf_frames * (size_t)w->channels, sizeof *w->buf);
        if (w->ctx_rate > 0 && w->ctx_rate != w->dev_rate) {
            int err = 0;
            w->rs = capture ? speex_resampler_init((spx_uint32_t)w->channels, (spx_uint32_t)w->ctx_rate, (spx_uint32_t)w->dev_rate, 5, &err)
                            : speex_resampler_init((spx_uint32_t)w->channels, (spx_uint32_t)w->dev_rate, (spx_uint32_t)w->ctx_rate, 5, &err);
            w->rs_cap = w->buf_frames * (size_t)(w->dev_rate > w->ctx_rate ? w->dev_rate / w->ctx_rate + 1 : 1);
            w->rs_buf = calloc(w->rs_cap * (size_t)w->channels, sizeof *w->rs_buf);
            w->fifo_cap = w->buf_frames * 4;
            w->fifo = calloc(w->fifo_cap * (size_t)w->channels, sizeof *w->fifo);
            if (!w->rs || !w->rs_buf || !w->fifo) { web_close(d); return OC_AUDIO_FAILED; }
            printf("oc-web: audio context at %d Hz for a %d Hz device: resampling\n", w->ctx_rate, w->dev_rate);
        }
        if (!w->buf) { web_close(d); return OC_AUDIO_FAILED; }
        struct open_args oa = { w, c, loopback, id ? id : "" };
        on_main(open_worklet_main, &oa);
    } else {
        w->buf_frames = 1024;                  /* ScriptProcessor's sizes: a power of two, 256..16384 */
        w->buf = calloc(w->buf_frames * (size_t)w->channels, sizeof *w->buf);
        if (!w->buf) { web_close(d); return OC_AUDIO_FAILED; }
        struct open_args oa = { w, NULL, loopback, id ? id : "" };
        on_main(open_sp_main, &oa);
    }
    for (int waited = 0; w->state == 0 && waited < 120000; waited += 20) wait_ms(20);
    if (w->state != 1) { int rc = w->state == 0 ? OC_AUDIO_FAILED : w->state; web_close(d); return rc; }
    if (capture && !loopback) oc_webdev_refresh();   /* allowed now: the labels are there */
    return OC_AUDIO_OK;
}

static const oc_audio_backend WEB = { web_list, web_open, web_close };
const oc_audio_backend *oc_audio_platform_backend(void) { return &WEB; }
