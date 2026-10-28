/* The audio backend over miniaudio (audio_backend.h): the desktops' devices,
 * and the phones' when they come. miniaudio is vendored as one header; its
 * implementation is compiled here and nowhere else. */
#define _POSIX_C_SOURCE 200809L
#include "audio_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#include "../../../third_party/miniaudio/miniaudio.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef struct {
    ma_context ctx;
    ma_device  dev;
    int        have_ctx, have_dev;
} ma_impl;

static int ma_list(int capture, oc_audio_device *out, int cap) {
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return OC_AUDIO_FAILED;
    ma_device_info *play, *capt;
    ma_uint32 np, nc;
    if (ma_context_get_devices(&ctx, &play, &np, &capt, &nc) != MA_SUCCESS) {
        ma_context_uninit(&ctx);
        return OC_AUDIO_FAILED;
    }
    ma_device_info *list = capture ? capt : play;
    ma_uint32 n = capture ? nc : np;
    int count = 0;
    for (ma_uint32 i = 0; i < n && count < cap; i++, count++) {
        oc_audio_device *o = &out[count];
        memset(o, 0, sizeof *o);
        /* The id is the raw ma_device_id bytes, hex-encoded, so it survives as a string. */
        const unsigned char *raw = (const unsigned char *)&list[i].id;
        _Static_assert(sizeof o->id > sizeof list[i].id * 2, "device id must fit hex-encoded");
        /* Trailing zero bytes are left off; parse_id zero-fills them back. */
        size_t len = sizeof list[i].id;
        while (len > 0 && raw[len - 1] == 0) len--;
        for (size_t k = 0; k < len; k++) snprintf(o->id + 2 * k, 3, "%02x", raw[k]);
        snprintf(o->name, sizeof o->name, "%s", list[i].name);
        o->is_default = list[i].isDefault ? 1 : 0;
    }
    ma_context_uninit(&ctx);
    return count;
}

static int parse_id(const char *hex, ma_device_id *id) {
    memset(id, 0, sizeof *id);
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > sizeof *id) return -1;
    unsigned char *raw = (unsigned char *)id;
    for (size_t k = 0; k < n / 2; k++) {
        unsigned v;
        if (sscanf(hex + 2 * k, "%2x", &v) != 1) return -1;
        raw[k] = (unsigned char)v;
    }
    return 0;
}

static void capture_cb(ma_device *dev, void *out, const void *in, ma_uint32 frames) {
    (void)out;
    if (!in || frames == 0) return;
    oc_audio_dev_capture_in(dev->pUserData, in, frames);
}

static void playback_cb(ma_device *dev, void *out, const void *in, ma_uint32 frames) {
    (void)in;
    oc_audio_dev_playback_out(dev->pUserData, out, frames);
}

static void ma_close(oc_audio_dev *d) {
    ma_impl *m = *oc_audio_dev_impl(d);
    if (!m) return;
    if (m->have_dev) ma_device_uninit(&m->dev);
    if (m->have_ctx) ma_context_uninit(&m->ctx);
    free(m);
    *oc_audio_dev_impl(d) = NULL;
}

static int ma_open(oc_audio_dev *d, int capture, int loopback, const char *id) {
    ma_impl *m = calloc(1, sizeof *m);
    if (!m) return OC_AUDIO_FAILED;
    *oc_audio_dev_impl(d) = m;
    if (ma_context_init(NULL, 0, NULL, &m->ctx) != MA_SUCCESS) { ma_close(d); return OC_AUDIO_FAILED; }
    m->have_ctx = 1;
    ma_device_id dev_id;
    int have_id = id && *id && parse_id(id, &dev_id) == 0;
    ma_device_config cfg = ma_device_config_init(loopback ? ma_device_type_loopback
                                                 : capture ? ma_device_type_capture : ma_device_type_playback);
    if (capture) {
        /* A loopback device is named by the OUTPUT it listens to; NULL is the default. */
        cfg.capture.pDeviceID = have_id ? &dev_id : NULL;
        cfg.capture.format = ma_format_s16;
        cfg.capture.channels = (ma_uint32)oc_audio_dev_channels(d);
        cfg.dataCallback = capture_cb;
    } else {
        cfg.playback.pDeviceID = have_id ? &dev_id : NULL;
        cfg.playback.format = ma_format_s16;
        cfg.playback.channels = (ma_uint32)oc_audio_dev_channels(d);
        cfg.dataCallback = playback_cb;
    }
    cfg.sampleRate = (ma_uint32)oc_audio_dev_rate(d);
    cfg.periodSizeInMilliseconds = 10;
    cfg.pUserData = d;
    ma_result r = ma_device_init(&m->ctx, &cfg, &m->dev);
    if (r != MA_SUCCESS) {
        ma_close(d);
        return r == MA_ACCESS_DENIED ? OC_AUDIO_DENIED : r == MA_NO_DEVICE ? OC_AUDIO_NODEVICE : OC_AUDIO_FAILED;
    }
    m->have_dev = 1;
    if (ma_device_start(&m->dev) != MA_SUCCESS) { ma_close(d); return OC_AUDIO_FAILED; }
    return OC_AUDIO_OK;
}

static const oc_audio_backend MA_BACKEND = { ma_list, ma_open, ma_close };
const oc_audio_backend *oc_audio_platform_backend(void) { return &MA_BACKEND; }
