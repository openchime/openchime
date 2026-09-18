/* The capture interface (REQ-163, ARCH-110, docs/VIDEO-MESSAGES.md §3): cameras,
 * and screens and windows. One interface for every platform; every backend
 * delivers oc_frame (planar I420, BT.709 limited range, even dimensions) stamped
 * from oc_media_clock_us(). */
#ifndef OC_CAPTURE_H
#define OC_CAPTURE_H

#include "oc_media.h"

enum {
    OC_CAP_OK       =  0,
    OC_CAP_DENIED   = -1,    /* the operating system blocks the camera */
    OC_CAP_NODEVICE = -2,
    OC_CAP_BUSY     = -3,    /* another application holds it */
    OC_CAP_FAILED   = -4,
    OC_CAP_GONE     = -5,    /* what was being captured went away: a window closed */
};

/* What a device is. */
enum { OC_SOURCE_CAMERA = 0, OC_SOURCE_SCREEN, OC_SOURCE_WINDOW };

typedef struct {
    char id[256];            /* backend-specific; pass to oc_capture_open */
    char name[128];          /* UTF-8, for a picker */
    int  kind;               /* OC_SOURCE_* */
} oc_capture_device;

typedef struct oc_capture oc_capture;

/* What a backend provides. The front functions below pick one and forward. */
typedef struct {
    int  (*list)(oc_capture_device *out, int cap);
    void *(*open)(const char *device_id, int want_w, int want_h, int want_fps, int *err);
    int  (*start)(void *impl);
    int  (*next)(void *impl, oc_frame *f, int timeout_ms);
    void (*stop)(void *impl);
    void (*close)(void *impl);
} oc_capture_backend;

/* The devices available, at most `cap`. Returns the count or a negative OC_CAP_*. */
int  oc_capture_list(oc_capture_device *out, int cap);
/* Open a device (NULL or "" for the default) asking for a size and rate; the
 * backend delivers the nearest it has, scaled to even dimensions. NULL on
 * failure with `*err` set to an OC_CAP_* code. */
oc_capture *oc_capture_open(const char *device_id, int want_w, int want_h, int want_fps, int *err);
int  oc_capture_start(oc_capture *c);
/* Wait up to `timeout_ms` for the next frame: 1 with `f` filled (its planes stay
 * valid until the next call), 0 on timeout, a negative OC_CAP_* on error. */
int  oc_capture_next(oc_capture *c, oc_frame *f, int timeout_ms);
void oc_capture_stop(oc_capture *c);
/* Release the device at once (REQ-166). */
void oc_capture_close(oc_capture *c);

/* Screens and windows (REQ-162): listed apart from the cameras, monitors first.
 * Returns the count, 0 where screen capture is not available. */
int  oc_capture_list_screens(oc_capture_device *out, int cap);
/* Open a screen or window. Frames come at one size for the whole capture: the
 * source's shape fitted inside max_w×max_h when it opened, even; a window that
 * is resized later is fitted into that size. A screen backend reports a frame
 * only when something changed, and this front end repeats the last one so that
 * frames still arrive at `fps` while nothing moves. */
oc_capture *oc_capture_open_screen(const char *device_id, int max_w, int max_h, int fps, int *err);

/* The backends. `OPENCHIME_TEST_CAPTURE=synthetic|denied` selects the synthetic
 * ones; otherwise the platform's. `synthetic-camera` makes only the camera
 * synthetic, so a real screen can be recorded with a known picture in its box. */
extern const oc_capture_backend oc_capture_synthetic;
extern const oc_capture_backend oc_capture_denied;
/* A synthetic screen: 2560×1600 of text-like stripes that change five times a
 * second, so fitting and the repeating of still frames are exercised.
 * `OPENCHIME_TEST_SCREEN_GONE_MS` makes it go away after that long. */
extern const oc_capture_backend oc_capture_synthetic_screen;
/* NULL where this platform's backend is not built yet. */
const oc_capture_backend *oc_capture_platform(void);
const oc_capture_backend *oc_capture_screen_platform(void);

/* Why the last open or read failed, in words a log can carry: the step and the
 * platform's code ("SetCurrentMediaType NV12: 0xC00D5212"). "" when nothing has
 * failed. Not thread-safe; for crumbs and diagnostics. */
const char *oc_capture_detail(void);
void        oc_capture_set_detail(const char *fmt, ...);

/* The synthetic pattern's frame number, read back from the counter blocks in a
 * (decoded) frame's top row, or -1 if the blocks are unreadable. */
int oc_capture_synthetic_frame_number(const oc_frame *f);

#endif
