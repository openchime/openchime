/* OpenChime web client -- the audio backend over Web Audio (audio_backend.h,
 * WEB.md). The page's thread owns an AudioContext per sample rate; a device
 * is a ScriptProcessorNode whose callback moves samples between the
 * browser's float buffers and wasm memory and hands them to the device layer
 * (oc_audio_dev_capture_in / oc_audio_dev_playback_out), which keeps the
 * rings every media thread reads and writes. A microphone is a getUserMedia
 * stream into such a node; a speaker is such a node into the destination.
 * The computer's own sound (loopback) is the audio track the browser gives
 * with a screen capture (cap_web.c): a tab's sound anywhere, the system's on
 * Chromium for Windows; where it gives none, the device is not there.
 *
 * Opening a microphone waits for the permission prompt; the waiting is
 * Asyncify's on the page's thread and a sleep on any other. */
#include "audio_backend.h"
#include "oc_media.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int      handle;
    int16_t *buf;        /* the callback's exchange buffer, in wasm memory */
    size_t   buf_frames;
    int      state;      /* written by the page's thread: 0 opening, 1 open, <0 OC_AUDIO_* */
} wdev;

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

/* The page's callbacks land here. */
EMSCRIPTEN_KEEPALIVE void oc_web_audio_in(oc_audio_dev *d, const int16_t *pcm, int frames) {
    oc_audio_dev_capture_in(d, pcm, (size_t)frames);
}
EMSCRIPTEN_KEEPALIVE void oc_web_audio_out(oc_audio_dev *d, int16_t *pcm, int frames) {
    oc_audio_dev_playback_out(d, pcm, (size_t)frames);
}

EM_JS(void, aud_js_open, (int handle, oc_audio_dev *dev, int capture, int loopback, const char *id, int rate, int channels,
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
                /* Autoplay policy: the context runs after a gesture; the next one, if this is not one. */
                const resume = () => { ctx.resume(); };
                document.addEventListener("pointerdown", resume, { once: true });
                document.addEventListener("keydown", resume, { once: true });
            }
            if (ctx.state !== "running") { try { await ctx.resume(); } catch (e) {} }
            const d = { ctx, nodes: [] };
            if (capture) {
                if (loopback) {
                    /* The computer's sound: the audio track of the screen being
                     * captured (cap_web.c), when the browser gave one. */
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
                /* A silent input keeps the node scheduled on every browser. */
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

EM_ASYNC_JS(int, aud_js_list, (int capture, char *out, int cap, int stride), {
    try {
        if (!navigator.mediaDevices) return 0;
        const all = await navigator.mediaDevices.enumerateDevices();
        const want = capture ? "audioinput" : "audiooutput";
        let n = 0;
        for (const d of all) {
            if (d.kind !== want || n >= cap) continue;
            stringToUTF8(d.deviceId || "default", out + n * stride, 520);
            stringToUTF8(d.label || ((capture ? "Microphone " : "Speaker ") + (n + 1)), out + n * stride + 520, 256);
            HEAP32[(out + n * stride + 776) >> 2] = d.deviceId === "default" || n === 0 ? 1 : 0;
            n++;
        }
        /* An output the browser does not enumerate (Firefox, Safari): the default. */
        if (!capture && n === 0 && cap > 0) {
            stringToUTF8("default", out, 520); stringToUTF8("Speakers", out + 520, 256);
            HEAP32[(out + 776) >> 2] = 1; n = 1;
        }
        return n;
    } catch (e) { return 0; }
});

static int web_list(int capture, oc_audio_device *out, int cap) {
    if (cap <= 0 || !emscripten_is_main_runtime_thread()) return 0;
    _Static_assert(sizeof(oc_audio_device) == 520 + 256 + sizeof(int), "the page writes the device record by offset");
    memset(out, 0, (size_t)cap * sizeof *out);
    return aud_js_list(capture, out->id, cap, (int)sizeof *out);
}

static void close_main(void *arg) { aud_js_close(*(int *)arg); }

static void web_close(oc_audio_dev *d) {
    wdev *w = *oc_audio_dev_impl(d);
    if (!w) return;
    on_main(close_main, &w->handle);
    free(w->buf);
    free(w);
    *oc_audio_dev_impl(d) = NULL;
}

struct open_args { int handle; oc_audio_dev *d; int capture, loopback; const char *id; int16_t *buf; int frames; int *state; };
static void open_main(void *arg) {
    struct open_args *o = arg;
    aud_js_open(o->handle, o->d, o->capture, o->loopback, o->id, oc_audio_dev_rate(o->d), oc_audio_dev_channels(o->d),
                o->buf, o->frames, o->state);
}

static int web_open(oc_audio_dev *d, int capture, int loopback, const char *id) {
    static int next_handle = 1;
    wdev *w = calloc(1, sizeof *w);
    if (!w) return OC_AUDIO_FAILED;
    w->handle = next_handle++;
    w->buf_frames = 1024;                      /* ScriptProcessor's sizes: a power of two, 256..16384 */
    w->buf = calloc(w->buf_frames * (size_t)oc_audio_dev_channels(d), sizeof *w->buf);
    if (!w->buf) { free(w); return OC_AUDIO_FAILED; }
    *oc_audio_dev_impl(d) = w;
    struct open_args oa = { w->handle, d, capture, loopback, id ? id : "", w->buf, (int)w->buf_frames, &w->state };
    on_main(open_main, &oa);
    for (int waited = 0; w->state == 0 && waited < 120000; waited += 20) wait_ms(20);
    if (w->state != 1) { int rc = w->state == 0 ? OC_AUDIO_FAILED : w->state; web_close(d); return rc; }
    return OC_AUDIO_OK;
}

static const oc_audio_backend WEB = { web_list, web_open, web_close };
const oc_audio_backend *oc_audio_platform_backend(void) { return &WEB; }
