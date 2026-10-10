/* OpenChime web client -- the capture backend (oc_capture.h, WEB.md): the
 * camera through getUserMedia, a screen or window through getDisplayMedia.
 *
 * A stream plays in a <video> the page never shows; each new frame the
 * browser decodes (requestVideoFrameCallback) is counted, and `next` draws
 * the newest one into a canvas and reads it back as RGBA into wasm memory,
 * where it becomes the I420 frame every backend delivers. The browser owns
 * the permission prompt and, for a screen, the picker: the one "screen"
 * device listed is whatever the person chooses there, and the capture ends
 * (OC_CAP_GONE) when they stop sharing from the browser's own bar. A screen
 * is asked for with its audio: a tab's sound, or the system's where the
 * browser can give it (Chromium on Windows), which the audio backend reads as
 * the computer's sound for a screen recording (audio_web.c).
 *
 * The page's thread owns the stream and the canvas. Opening runs where the
 * application calls it (its thread, while the click that asked is still a
 * user gesture, which getDisplayMedia requires); `next` is called from the
 * recorder's thread and proxies the draw to the page's thread. */
#include "oc_capture.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int      handle;
    int      w, h, fps;
    int      screen;
    uint8_t *rgba;
    oc_frame out;
    int      running;
    /* Written by the page's thread: 0 opening, 1 open, <0 an OC_CAP_* error. */
    int      state;
} wcap;

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

EM_JS(void, cap_js_open, (int handle, const char *id, int want_w, int want_h, int fps, int screen, int *state), {
    const reg = Module.ocCap || (Module.ocCap = { next: 1, s: {} });
    const dev = UTF8ToString(id);
    const even = (v) => Math.max(2, v & ~1);
    const fail = (e) => {
        const n = e && e.name;
        const code = n === "NotAllowedError" || n === "SecurityError" ? -1
                   : n === "NotFoundError" || n === "OverconstrainedError" ? -2
                   : n === "NotReadableError" || n === "AbortError" ? -3 : -4;
        console.warn("capture: " + (e && e.message ? e.message : e));
        HEAP32[state >> 2] = code;
    };
    (async () => {
        try {
            let stream;
            if (screen) {
                /* With its sound, where the browser offers it (a tab's, or the
                 * system's on Windows): the recorder's "computer sound" reads
                 * it as the loopback device (audio_web.c). */
                stream = await navigator.mediaDevices.getDisplayMedia({ video: { frameRate: fps }, audio: true });
            } else {
                const v = { width: { ideal: want_w }, height: { ideal: want_h }, frameRate: { ideal: fps } };
                if (dev && dev !== "default") v.deviceId = { exact: dev };
                stream = await navigator.mediaDevices.getUserMedia({ video: v, audio: false });
            }
            const video = document.createElement("video");
            video.muted = true; video.playsInline = true; video.srcObject = stream;
            video.style.cssText = "position:fixed;left:-10000px;top:0;width:1px;height:1px";
            document.body.appendChild(video);
            await video.play();
            let vw = video.videoWidth || want_w, vh = video.videoHeight || want_h, w, h;
            if (screen) {
                const sc = Math.min(want_w / vw, want_h / vh, 1);
                w = even(Math.round(vw * sc)); h = even(Math.round(vh * sc));
            } else {
                w = even(vw); h = even(vh);
            }
            const canvas = document.createElement("canvas");
            canvas.width = w; canvas.height = h;
            const ctx = canvas.getContext("2d", { willReadFrequently: true });
            const s = { stream, video, canvas, ctx, w, h, frames: 0, taken: 0, gone: 0, screen: !!screen, opened: Date.now() };
            reg.s[handle] = s;
            const track = stream.getVideoTracks()[0];
            if (track) track.addEventListener("ended", () => { s.gone = 1; });
            if (video.requestVideoFrameCallback) {
                const tick = () => { s.frames++; if (reg.s[handle] === s) video.requestVideoFrameCallback(tick); };
                video.requestVideoFrameCallback(tick);
            } else {
                let last = -1;
                const poll = () => { if (reg.s[handle] !== s) return; if (video.currentTime !== last) { last = video.currentTime; s.frames++; } setTimeout(poll, 1000 / Math.max(1, fps)); };
                poll();
            }
            HEAP32[(state >> 2) + 1] = w; HEAP32[(state >> 2) + 2] = h;
            HEAP32[state >> 2] = 1;
        } catch (e) { fail(e); }
    })();
});

EM_JS(int, cap_js_grab, (int handle, uint8_t *rgba), {
    const reg = Module.ocCap; const s = reg && reg.s[handle];
    if (!s) return -4;
    if (s.gone) return -5;
    if (s.frames === s.taken || s.video.readyState < 2) return 0;
    s.taken = s.frames;
    try {
        s.ctx.drawImage(s.video, 0, 0, s.w, s.h);
        const img = s.ctx.getImageData(0, 0, s.w, s.h);
        HEAPU8.set(img.data, rgba);
        return 1;
    } catch (e) { return -4; }
});

EM_JS(void, cap_js_close, (int handle), {
    const reg = Module.ocCap; const s = reg && reg.s[handle];
    if (!s) return;
    delete reg.s[handle];
    try { s.stream.getTracks().forEach((t) => t.stop()); } catch (e) {}
    try { s.video.pause(); s.video.srcObject = null; s.video.remove(); } catch (e) {}
});

/* The cameras: enumerateDevices, on the page's thread (the application's).
 * Labels are the browser's once a camera has been allowed; before that,
 * numbered. */
EM_ASYNC_JS(int, cap_js_list, (char *out, int cap, int stride, int screen), {
    try {
        if (!navigator.mediaDevices) return 0;
        if (screen) {
            stringToUTF8("display", out, 256);
            stringToUTF8("Your screen or a window", out + 256, 128);
            return 1;
        }
        const all = await navigator.mediaDevices.enumerateDevices();
        let n = 0;
        for (const d of all) {
            if (d.kind !== "videoinput" || n >= cap) continue;
            stringToUTF8(d.deviceId || "default", out + n * stride, 256);
            stringToUTF8(d.label || ("Camera " + (n + 1)), out + n * stride + 256, 128);
            n++;
        }
        return n;
    } catch (e) { return 0; }
});

static int list_any(oc_capture_device *out, int cap, int screen) {
    if (cap <= 0 || !emscripten_is_main_runtime_thread()) return 0;
    memset(out, 0, (size_t)cap * sizeof *out);
    int n = cap_js_list(out->id, cap, (int)sizeof *out, screen);
    for (int i = 0; i < n; i++) out[i].kind = screen ? OC_SOURCE_SCREEN : OC_SOURCE_CAMERA;
    return n;
}
static int cam_list(oc_capture_device *out, int cap) { return list_any(out, cap, 0); }
static int scr_list(oc_capture_device *out, int cap) { return list_any(out, cap, 1); }

struct open_args { int handle; const char *id; int w, h, fps, screen; int *state; };
static void open_main(void *arg) {
    struct open_args *o = arg;
    cap_js_open(o->handle, o->id, o->w, o->h, o->fps, o->screen, o->state);
}

static void *open_any(const char *device_id, int want_w, int want_h, int want_fps, int *err, int screen) {
    static int next_handle = 1;
    wcap *c = calloc(1, sizeof *c);
    if (!c) { *err = OC_CAP_FAILED; return NULL; }
    c->handle = next_handle++;
    c->fps = want_fps > 0 ? want_fps : 30;
    c->screen = screen;
    /* state, then w and h, filled by the page. */
    int st[3] = { 0, 0, 0 };
    struct open_args oa = { c->handle, device_id ? device_id : "", want_w > 0 ? want_w : 1280,
                            want_h > 0 ? want_h : 720, c->fps, screen, st };
    on_main(open_main, &oa);
    for (int waited = 0; st[0] == 0 && waited < 120000; waited += 20) wait_ms(20);
    if (st[0] != 1) {
        *err = st[0] == 0 ? OC_CAP_FAILED : st[0];
        oc_capture_set_detail("getUserMedia: %d", st[0]);
        cap_js_close(c->handle);
        free(c);
        return NULL;
    }
    c->w = st[1]; c->h = st[2];
    c->rgba = malloc((size_t)c->w * (size_t)c->h * 4);
    if (!c->rgba || oc_frame_alloc(&c->out, c->w, c->h) != 0) {
        *err = OC_CAP_FAILED; cap_js_close(c->handle); free(c->rgba); free(c); return NULL;
    }
    *err = OC_CAP_OK;
    return c;
}
static void *cam_open(const char *id, int w, int h, int fps, int *err) { return open_any(id, w, h, fps, err, 0); }
static void *scr_open(const char *id, int w, int h, int fps, int *err) { return open_any(id, w, h, fps, err, 1); }

static int cap_start(void *impl) { ((wcap *)impl)->running = 1; return OC_CAP_OK; }
static void cap_stop(void *impl) { ((wcap *)impl)->running = 0; }

struct grab { int handle; uint8_t *rgba; int r; };
static void grab_main(void *arg) { struct grab *g = arg; g->r = cap_js_grab(g->handle, g->rgba); }
static void close_main(void *arg) { cap_js_close(*(int *)arg); }

static int cap_next(void *impl, oc_frame *f, int timeout_ms) {
    wcap *c = impl;
    int waited = 0;
    for (;;) {
        if (!c->running) return 0;
        struct grab g = { c->handle, c->rgba, 0 };
        on_main(grab_main, &g);
        int r = g.r;
        if (r == 1) {
            oc_i420_from_rgba(&c->out, c->rgba, c->w * 4);
            c->out.pts_us = oc_media_clock_us();
            *f = c->out;
            return 1;
        }
        if (r < 0) return r;
        if (waited >= timeout_ms) return 0;
        wait_ms(5);
        waited += 5;
    }
}

static void cap_close(void *impl) {
    wcap *c = impl;
    on_main(close_main, &c->handle);
    oc_frame_free(&c->out);
    free(c->rgba);
    free(c);
}

static const oc_capture_backend CAM = { cam_list, cam_open, cap_start, cap_next, cap_stop, cap_close };
static const oc_capture_backend SCR = { scr_list, scr_open, cap_start, cap_next, cap_stop, cap_close };
const oc_capture_backend *oc_capture_platform(void) { return &CAM; }
const oc_capture_backend *oc_capture_screen_platform(void) { return &SCR; }
