/* Recording a video message (REQ-162, ARCH-110, docs/VIDEO-MESSAGES.md §4): a
 * camera -- or a screen or window, with the camera in a box -- and a microphone
 * and the computer's sound in, a VP9 + Opus MP4 and a JPEG poster out, both in
 * memory, since a client writes no files (ARCH-88).
 *
 * Opening a recorder opens the devices and starts the preview; nothing is
 * encoded until oc_recorder_start. Closing it releases both devices at once. */
#ifndef OC_RECORDER_H
#define OC_RECORDER_H

#include <stddef.h>
#include <stdint.h>

#define OC_RECORDER_CAP_MS 300000u          /* five minutes (REQ-162) */
/* Stop before the file could outgrow the daemon's default video cap
 * (OC_MAX_VIDEO_MESSAGE_SIZE, 160 MiB): the moov and the last packets still fit. */
#define OC_RECORDER_MAX_BYTES (155u * 1024u * 1024u)

/* The quality a user picks: the height recorded at, 16:9. A camera that cannot
 * deliver it gives the nearest it has, and anything larger is scaled down. */
enum { OC_REC_Q360 = 360, OC_REC_Q720 = 720, OC_REC_Q1080 = 1080 };

enum {
    OC_REC_OK              =  0,
    OC_REC_CAMERA_DENIED   = -1,
    OC_REC_CAMERA_NODEVICE = -2,
    OC_REC_CAMERA_BUSY     = -3,
    OC_REC_MIC_DENIED      = -4,
    OC_REC_FAILED          = -5,
    /* Voice input holds the microphone; it has one owner at a time (ARCH-112). */
    OC_REC_MIC_BUSY        = -6,
    /* A screen recording: the system refused the capture; this Windows cannot
     * capture screens; the screen or window is gone. */
    OC_REC_SCREEN_DENIED   = -7,
    OC_REC_SCREEN_UNSUPPORTED = -8,
    OC_REC_SCREEN_GONE     = -9,
};

typedef enum {
    OC_REC_PREVIEW,          /* devices open, nothing recorded */
    OC_REC_RECORDING,
    OC_REC_FINISHING,        /* stopped; flushing and assembling the file */
    OC_REC_DONE,             /* oc_recorder_take has a result */
    OC_REC_ERROR,
} oc_rec_state;

typedef struct {
    const char *camera_id;   /* NULL or "" for the default */
    const char *mic_id;
    uint32_t    cap_ms;      /* 0 for OC_RECORDER_CAP_MS; OPENCHIME_TEST_VIDEO_CAP_MS overrides */
    int         width, height, fps;   /* 0 for 1280×720 at 30; a screen fits inside the size */
    uint64_t    max_bytes;   /* 0 for OC_RECORDER_MAX_BYTES */
    /* A screen recording: a screen or window from oc_capture_list_screens, NULL
     * or "" to record the camera. With `with_camera`, camera_id's camera is
     * boxed into `corner` (OC_CORNER_*). `computer_sound` adds what the
     * computer plays, mixed with the microphone. */
    const char *screen_id;
    int         with_camera;
    int         corner;
    int         computer_sound;
} oc_recorder_opts;

typedef struct {
    oc_rec_state state;
    uint32_t     elapsed_ms;
    uint32_t     cap_ms;
    int          level;              /* microphone peak, 0..32767 */
    int          has_audio;          /* a microphone is recording */
    int          stepped_down;       /* the encoder fell behind and dropped a size */
    int          enc_width, enc_height;   /* what is being encoded */
    uint64_t     dropped_frames;
    int          screen;             /* recording a screen or window */
    int          computer_sound;     /* the computer's sound is being recorded */
    int          source_gone;        /* the window closed; what came before was kept */
} oc_rec_status;

typedef struct {
    uint8_t *video;   size_t video_len;    /* the MP4 */
    uint8_t *poster;  size_t poster_len;   /* the JPEG */
    uint32_t duration_ms;
    int      width, height;
} oc_rec_result;

typedef struct oc_recorder oc_recorder;

/* Open the devices and begin the preview. NULL on failure with `*err` an
 * OC_REC_* code. A machine with no microphone records video alone; a
 * microphone the OS denies is an error (the user should be told, not
 * silently recorded without sound). */
oc_recorder *oc_recorder_open(const oc_recorder_opts *opts, int *err);

/* Copy the latest camera frame as BGRA into `bgra` (`cap` bytes) if it is newer
 * than `*seq`. Returns 1 with `*w`, `*h`, `*seq` updated, 0 when nothing is
 * new, -1 when `cap` is too small (`*w`, `*h` still set). */
int oc_recorder_preview(oc_recorder *r, uint8_t *bgra, size_t cap, int *w, int *h, uint64_t *seq);

int  oc_recorder_start(oc_recorder *r);
/* Stop recording and assemble the file on the encode thread. */
void oc_recorder_stop(oc_recorder *r);
void oc_recorder_status(oc_recorder *r, oc_rec_status *st);
/* Once DONE: move the result out (0), leaving nothing in the recorder. */
int  oc_recorder_take(oc_recorder *r, oc_rec_result *out);
/* Stop everything, release the devices, free any result not taken. */
void oc_recorder_close(oc_recorder *r);

void oc_rec_result_free(oc_rec_result *res);

/* The poster rule: the index of the first video keyframe at or after 1 s, else
 * the first keyframe. */
struct oc_mp4_info;
int oc_recorder_poster_index(const struct oc_mp4_info *info);

/* Decode that keyframe out of an MP4 in memory and encode it as JPEG into
 * `*jpeg` (malloc'd). Returns 0 or -1. */
int oc_recorder_poster_jpeg(const uint8_t *mp4, size_t len, uint8_t **jpeg, size_t *jpeg_len,
                            int *width, int *height);

#endif
