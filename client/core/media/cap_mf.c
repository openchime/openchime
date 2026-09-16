/* The Windows capture backend: Media Foundation's source reader
 * (docs/VIDEO-MESSAGES.md §3.2). Video processing is enabled so the reader
 * converts whatever the camera speaks (YUY2, MJPEG, NV12) to NV12, which is then
 * repacked into I420.
 *
 * The reader runs asynchronously. A blocking ReadSample on a camera that stops
 * delivering never returns and cannot be cancelled, so closing would hang; with
 * a callback, each sample arrives on Media Foundation's own thread, the next is
 * requested only while the device is running, and closing flushes and waits for
 * the callback to go quiet. The callback object is reference-counted apart from
 * the capture, and forgets it under its own lock, so a late call after close
 * touches nothing that has been freed. */
#ifdef _WIN32
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "oc_capture.h"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIRST_VIDEO ((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM)
#define MF_FAIL(what, hr) oc_capture_set_detail("%s: 0x%08lX", what, (unsigned long)(hr))

typedef struct mfcap mfcap;

/* The IMFSourceReaderCallback object. */
typedef struct {
    IMFSourceReaderCallback iface;
    LONG             ref;
    CRITICAL_SECTION mu;
    mfcap           *owner;              /* NULL once the capture is closing */
} mfcb;

struct mfcap {
    IMFMediaSource   *source;
    IMFSourceReader  *reader;
    mfcb             *cb;
    int               width, height;       /* delivered (even) */
    int               src_w, src_h;        /* the reader's output */
    int               stride;              /* NV12 luma pitch when the buffer has no 2D interface */
    CRITICAL_SECTION  mu;
    CONDITION_VARIABLE cv;
    int               com_ours;            /* mf_up initialised COM for the opening thread */
    int               running, failed;
    int               pending;             /* a ReadSample is outstanding */
    int               flushed;
    oc_frame          ready, out;          /* the newest frame; the one handed out */
    uint64_t          ready_seq, out_seq;
};

static LONG g_mf_users;

/* COM on the calling thread plus Media Foundation, reference-counted. Returns 1
 * when this call initialised COM and mf_down must undo it, 0 when the thread
 * already had COM in another apartment (the UI thread is STA for WIC), -1 on
 * failure. Uninitialising a COM this call did not initialise would tear down
 * the thread's own. */
static int mf_up(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int ours = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return -1;
    if (InterlockedIncrement(&g_mf_users) == 1) {
        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) { InterlockedDecrement(&g_mf_users); if (ours) CoUninitialize(); return -1; }
    }
    return ours;
}

static void mf_down(int ours) {
    if (InterlockedDecrement(&g_mf_users) == 0) MFShutdown();
    if (ours) CoUninitialize();
}

static void utf8(const WCHAR *w, char *out, int cap) {
    out[0] = 0;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    out[cap - 1] = 0;
}

static int map_hr(HRESULT hr) {
    if (hr == E_ACCESSDENIED) return OC_CAP_DENIED;
    if (hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
        hr == MF_E_HW_MFT_FAILED_START_STREAMING)
        return OC_CAP_BUSY;
    return OC_CAP_FAILED;
}

static HRESULT enum_devices(IMFActivate ***acts, UINT32 *count) {
    IMFAttributes *attr = NULL;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr))
        hr = IMFAttributes_SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(attr, acts, count);
    if (attr) IMFAttributes_Release(attr);
    return hr;
}

static void free_devices(IMFActivate **acts, UINT32 count) {
    for (UINT32 i = 0; i < count; i++) IMFActivate_Release(acts[i]);
    CoTaskMemFree(acts);
}

static int mf_list(oc_capture_device *out, int cap) {
    int ours = mf_up();
    if (ours < 0) return OC_CAP_FAILED;
    IMFActivate **acts = NULL;
    UINT32 count = 0;
    int n = 0;
    if (SUCCEEDED(enum_devices(&acts, &count))) {
        for (UINT32 i = 0; i < count && n < cap; i++) {
            WCHAR *name = NULL, *link = NULL;
            UINT32 len;
            IMFActivate_GetAllocatedString(acts[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
            IMFActivate_GetAllocatedString(acts[i], &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
            if (link) {
                memset(&out[n], 0, sizeof out[n]);
                utf8(link, out[n].id, sizeof out[n].id);
                utf8(name ? name : L"Camera", out[n].name, sizeof out[n].name);
                n++;
            }
            CoTaskMemFree(name);
            CoTaskMemFree(link);
        }
        free_devices(acts, count);
    }
    mf_down(ours);
    return n;
}

/* The native type nearest the wanted size and rate: size first, then rate, with
 * a mild preference for formats the processor converts cheaply. */
static IMFMediaType *pick_native(IMFSourceReader *reader, int want_w, int want_h, int want_fps) {
    IMFMediaType *best = NULL;
    double best_score = 1e30;
    for (DWORD i = 0;; i++) {
        IMFMediaType *t = NULL;
        if (FAILED(IMFSourceReader_GetNativeMediaType(reader, FIRST_VIDEO, i, &t))) break;
        UINT64 size = 0, rate = 0;
        GUID sub;
        IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &size);
        IMFMediaType_GetUINT64(t, &MF_MT_FRAME_RATE, &rate);
        IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub);
        int w = (int)(size >> 32), h = (int)(size & 0xFFFFFFFF);
        UINT32 num = (UINT32)(rate >> 32), den = (UINT32)(rate & 0xFFFFFFFF);
        double fps = den ? (double)num / den : 0;
        double score = fabs((double)w * h - (double)want_w * want_h) / 1000.0
                     + (fps + 0.5 < want_fps ? (want_fps - fps) * 400.0 : (fps - want_fps) * 5.0);
        if (!IsEqualGUID(&sub, &MFVideoFormat_NV12) && !IsEqualGUID(&sub, &MFVideoFormat_YUY2)) score += 50.0;
        if (w <= 0 || h <= 0) score = 1e29;
        if (score < best_score) {
            if (best) IMFMediaType_Release(best);
            best = t; best_score = score;
        } else {
            IMFMediaType_Release(t);
        }
    }
    return best;
}

static void take_sample(mfcap *c, IMFSample *sample, int64_t now) {
    IMFMediaBuffer *buf = NULL;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buf))) return;
    IMF2DBuffer *b2 = NULL;
    BYTE *p = NULL;
    LONG pitch = 0;
    DWORD len = 0;
    int locked2d = SUCCEEDED(IMFMediaBuffer_QueryInterface(buf, &IID_IMF2DBuffer, (void **)&b2)) &&
                   SUCCEEDED(IMF2DBuffer_Lock2D(b2, &p, &pitch));
    if (!locked2d) {
        if (b2) { IMF2DBuffer_Release(b2); b2 = NULL; }
        if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len))) pitch = c->stride;
        else p = NULL;
    }
    /* NV12: the chroma plane starts after the luma rows at the same pitch. A
     * negative pitch (bottom-up) does not occur for YUV formats. */
    if (p && pitch >= c->width && (locked2d || len >= (DWORD)pitch * (DWORD)c->src_h * 3 / 2)) {
        EnterCriticalSection(&c->mu);
        oc_i420_from_nv12(&c->ready, p, pitch, p + pitch * c->src_h, pitch);
        c->ready.pts_us = now;
        c->ready_seq++;
        WakeAllConditionVariable(&c->cv);
        LeaveCriticalSection(&c->mu);
    }
    if (locked2d) { IMF2DBuffer_Unlock2D(b2); IMF2DBuffer_Release(b2); }
    else if (p) IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_Release(buf);
}

static HRESULT STDMETHODCALLTYPE cb_qi(IMFSourceReaderCallback *This, REFIID riid, void **out) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFSourceReaderCallback)) {
        *out = This;
        IMFSourceReaderCallback_AddRef(This);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cb_addref(IMFSourceReaderCallback *This) {
    return (ULONG)InterlockedIncrement(&((mfcb *)This)->ref);
}
static ULONG STDMETHODCALLTYPE cb_release(IMFSourceReaderCallback *This) {
    mfcb *cb = (mfcb *)This;
    LONG n = InterlockedDecrement(&cb->ref);
    if (n == 0) { DeleteCriticalSection(&cb->mu); free(cb); }
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE cb_on_read(IMFSourceReaderCallback *This, HRESULT hr, DWORD stream,
                                           DWORD flags, LONGLONG ts, IMFSample *sample) {
    (void)stream; (void)ts;
    mfcb *cb = (mfcb *)This;
    int64_t now = oc_media_clock_us();
    EnterCriticalSection(&cb->mu);
    mfcap *c = cb->owner;
    if (c) {
        int bad = FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM));
        if (bad) {
            if (FAILED(hr)) MF_FAIL("ReadSample", hr);
            else oc_capture_set_detail("ReadSample flags 0x%lX", (unsigned long)flags);
        }
        if (!bad && sample) take_sample(c, sample, now);
        EnterCriticalSection(&c->mu);
        c->pending = 0;
        if (bad) c->failed = 1;
        int again = !bad && c->running;
        if (again) c->pending = 1;
        WakeAllConditionVariable(&c->cv);
        LeaveCriticalSection(&c->mu);
        if (again && FAILED(IMFSourceReader_ReadSample(c->reader, FIRST_VIDEO, 0, NULL, NULL, NULL, NULL))) {
            EnterCriticalSection(&c->mu);
            c->pending = 0; c->failed = 1;
            WakeAllConditionVariable(&c->cv);
            LeaveCriticalSection(&c->mu);
        }
    }
    LeaveCriticalSection(&cb->mu);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_on_flush(IMFSourceReaderCallback *This, DWORD stream) {
    (void)stream;
    mfcb *cb = (mfcb *)This;
    EnterCriticalSection(&cb->mu);
    mfcap *c = cb->owner;
    if (c) {
        EnterCriticalSection(&c->mu);
        c->flushed = 1;
        c->pending = 0;
        WakeAllConditionVariable(&c->cv);
        LeaveCriticalSection(&c->mu);
    }
    LeaveCriticalSection(&cb->mu);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_on_event(IMFSourceReaderCallback *This, DWORD stream, IMFMediaEvent *ev) {
    (void)This; (void)stream; (void)ev;
    return S_OK;
}

static IMFSourceReaderCallbackVtbl cb_vtbl = {
    cb_qi, cb_addref, cb_release, cb_on_read, cb_on_flush, cb_on_event,
};

static void mf_close(void *impl);

static void *mf_open(const char *device_id, int want_w, int want_h, int want_fps, int *err) {
    int ours = mf_up();
    if (ours < 0) { *err = OC_CAP_FAILED; return NULL; }
    mfcap *c = calloc(1, sizeof *c);
    if (!c) { mf_down(ours); *err = OC_CAP_FAILED; return NULL; }
    c->com_ours = ours;
    HRESULT hr;
    InitializeCriticalSection(&c->mu);
    InitializeConditionVariable(&c->cv);

    IMFActivate **acts = NULL;
    UINT32 count = 0;
    hr = enum_devices(&acts, &count);
    if (FAILED(hr) || count == 0) {
        if (SUCCEEDED(hr)) free_devices(acts, count);
        *err = FAILED(hr) ? map_hr(hr) : OC_CAP_NODEVICE;
        if (FAILED(hr)) MF_FAIL("MFEnumDeviceSources", hr); else oc_capture_set_detail("no camera");
        mf_close(c);
        return NULL;
    }
    UINT32 pick = 0;
    if (device_id && *device_id) {
        pick = UINT32_MAX;
        for (UINT32 i = 0; i < count && pick == UINT32_MAX; i++) {
            WCHAR *link = NULL; UINT32 len; char id[256];
            if (SUCCEEDED(IMFActivate_GetAllocatedString(acts[i], &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len))) {
                utf8(link, id, sizeof id);
                if (strcmp(id, device_id) == 0) pick = i;
                CoTaskMemFree(link);
            }
        }
        if (pick == UINT32_MAX) { free_devices(acts, count); *err = OC_CAP_NODEVICE; mf_close(c); return NULL; }
    }
    hr = IMFActivate_ActivateObject(acts[pick], &IID_IMFMediaSource, (void **)&c->source);
    free_devices(acts, count);
    if (FAILED(hr)) { MF_FAIL("ActivateObject", hr); *err = map_hr(hr); mf_close(c); return NULL; }

    c->cb = calloc(1, sizeof *c->cb);
    if (!c->cb) { *err = OC_CAP_FAILED; mf_close(c); return NULL; }
    c->cb->iface.lpVtbl = &cb_vtbl;
    c->cb->ref = 1;
    InitializeCriticalSection(&c->cb->mu);
    c->cb->owner = c;

    IMFAttributes *ra = NULL;
    hr = MFCreateAttributes(&ra, 2);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUINT32(ra, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = IMFAttributes_SetUnknown(ra, &MF_SOURCE_READER_ASYNC_CALLBACK, (IUnknown *)&c->cb->iface);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(c->source, ra, &c->reader);
    if (ra) IMFAttributes_Release(ra);
    if (FAILED(hr)) { MF_FAIL("MFCreateSourceReaderFromMediaSource", hr); *err = map_hr(hr); mf_close(c); return NULL; }

    IMFMediaType *native = pick_native(c->reader, want_w, want_h, want_fps);
    if (!native) { oc_capture_set_detail("no native video type"); *err = OC_CAP_FAILED; mf_close(c); return NULL; }
    hr = IMFSourceReader_SetCurrentMediaType(c->reader, FIRST_VIDEO, NULL, native);
    UINT64 size = 0;
    IMFMediaType_GetUINT64(native, &MF_MT_FRAME_SIZE, &size);
    IMFMediaType_Release(native);
    if (FAILED(hr)) { MF_FAIL("SetCurrentMediaType native", hr); *err = map_hr(hr); mf_close(c); return NULL; }

    /* Ask for NV12 at the native size; the reader's processor converts. */
    IMFMediaType *want = NULL;
    hr = MFCreateMediaType(&want);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = IMFMediaType_SetUINT64(want, &MF_MT_FRAME_SIZE, size);
    if (SUCCEEDED(hr)) hr = IMFSourceReader_SetCurrentMediaType(c->reader, FIRST_VIDEO, NULL, want);
    if (want) IMFMediaType_Release(want);
    if (FAILED(hr)) { MF_FAIL("SetCurrentMediaType NV12", hr); *err = map_hr(hr); mf_close(c); return NULL; }

    IMFMediaType *cur = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(c->reader, FIRST_VIDEO, &cur))) { *err = OC_CAP_FAILED; mf_close(c); return NULL; }
    IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &size);
    UINT32 stride = 0;
    if (FAILED(IMFMediaType_GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &stride))) stride = 0;
    IMFMediaType_Release(cur);
    c->src_w = (int)(size >> 32); c->src_h = (int)(size & 0xFFFFFFFF);
    c->stride = (int)stride > 0 ? (int)stride : c->src_w;
    /* Odd sizes lose their last row or column. */
    c->width = c->src_w & ~1; c->height = c->src_h & ~1;
    if (c->width < 2 || c->height < 2 ||
        oc_frame_alloc(&c->ready, c->width, c->height) != 0 ||
        oc_frame_alloc(&c->out, c->width, c->height) != 0) { *err = OC_CAP_FAILED; mf_close(c); return NULL; }

    *err = OC_CAP_OK;
    return c;
}

static int mf_start(void *impl) {
    mfcap *c = impl;
    EnterCriticalSection(&c->mu);
    int request = !c->running && !c->pending && !c->failed;
    c->running = 1;
    if (request) c->pending = 1;
    LeaveCriticalSection(&c->mu);
    if (request && FAILED(IMFSourceReader_ReadSample(c->reader, FIRST_VIDEO, 0, NULL, NULL, NULL, NULL))) {
        EnterCriticalSection(&c->mu);
        c->pending = 0; c->failed = 1;
        LeaveCriticalSection(&c->mu);
        return OC_CAP_FAILED;
    }
    return OC_CAP_OK;
}

static int mf_next(void *impl, oc_frame *f, int timeout_ms) {
    mfcap *c = impl;
    ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);
    EnterCriticalSection(&c->mu);
    while (!c->failed && c->ready_seq == c->out_seq) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) { LeaveCriticalSection(&c->mu); return 0; }
        SleepConditionVariableCS(&c->cv, &c->mu, (DWORD)(deadline - now));
    }
    if (c->failed) { LeaveCriticalSection(&c->mu); return OC_CAP_FAILED; }
    oc_frame_copy(&c->out, &c->ready);
    c->out_seq = c->ready_seq;
    LeaveCriticalSection(&c->mu);
    *f = c->out;
    return 1;
}

static void mf_stop(void *impl) {
    mfcap *c = impl;
    EnterCriticalSection(&c->mu);
    c->running = 0;                        /* the callback stops asking for more */
    LeaveCriticalSection(&c->mu);
}

static void mf_close(void *impl) {
    mfcap *c = impl;
    if (c->reader) {
        /* No new requests; then let an outstanding one land, flushing it out if
         * the camera has gone quiet. Both waits are bounded, and the callback
         * forgets the capture below whatever they end in. */
        EnterCriticalSection(&c->mu);
        c->running = 0;
        ULONGLONG deadline = GetTickCount64() + 500;
        while (c->pending && GetTickCount64() < deadline)
            SleepConditionVariableCS(&c->cv, &c->mu, 50);
        int still = c->pending;
        LeaveCriticalSection(&c->mu);
        if (still && SUCCEEDED(IMFSourceReader_Flush(c->reader, FIRST_VIDEO))) {
            EnterCriticalSection(&c->mu);
            deadline = GetTickCount64() + 2000;
            while (!c->flushed && GetTickCount64() < deadline)
                SleepConditionVariableCS(&c->cv, &c->mu, 50);
            LeaveCriticalSection(&c->mu);
        }
    }
    if (c->cb) {
        EnterCriticalSection(&c->cb->mu);
        c->cb->owner = NULL;
        LeaveCriticalSection(&c->cb->mu);
    }
    if (c->reader) IMFSourceReader_Release(c->reader);
    if (c->source) { IMFMediaSource_Shutdown(c->source); IMFMediaSource_Release(c->source); }
    if (c->cb) IMFSourceReaderCallback_Release(&c->cb->iface);
    oc_frame_free(&c->ready);
    oc_frame_free(&c->out);
    DeleteCriticalSection(&c->mu);
    int ours = c->com_ours;
    free(c);
    mf_down(ours);
}

static const oc_capture_backend mf_backend = { mf_list, mf_open, mf_start, mf_next, mf_stop, mf_close };

const oc_capture_backend *oc_capture_platform(void) { return &mf_backend; }

#endif /* _WIN32 */
