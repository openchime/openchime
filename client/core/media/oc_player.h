/* Playing a video message (REQ-165, ARCH-110, docs/VIDEO-MESSAGES.md §8): an
 * MP4 in memory in, BGRA frames for the UI and PCM to the speaker out.
 *
 * The audio clock is master: a video frame is presented once the samples the
 * speaker has consumed reach its timestamp, and a frame that is already a frame
 * late when the next one is due is decoded but never shown. A decoder so slow
 * that it falls further behind than that skips ahead to the next keyframe the
 * clock has passed. A file with no
 * audio track, or a machine with no speaker, runs on the monotonic clock. */
#ifndef OC_PLAYER_H
#define OC_PLAYER_H

#include <stddef.h>
#include <stdint.h>

typedef enum { OC_PLAYER_PAUSED, OC_PLAYER_PLAYING, OC_PLAYER_ENDED, OC_PLAYER_ERROR } oc_player_state;

typedef struct {
    oc_player_state state;
    uint32_t position_ms, duration_ms;   /* the clock */
    uint32_t frame_ms;               /* the presented frame's time */
    int      width, height;          /* the track's declared size */
    int      has_audio;              /* audio is going to a device */
    uint64_t presented, dropped;     /* video frames */
} oc_player_status;

typedef struct oc_player oc_player;

/* Open an MP4 held in memory. `mp4` is borrowed and must outlive the player.
 * Opens paused at the first frame. NULL if the file is not a video message. */
oc_player *oc_player_open(const uint8_t *mp4, size_t len);

void oc_player_play(oc_player *p);
void oc_player_pause(oc_player *p);
/* Seek to the video keyframe at or before `ms`; playback state is kept. */
void oc_player_seek(oc_player *p, uint32_t ms);
void oc_player_volume(oc_player *p, float gain);
void oc_player_status_get(oc_player *p, oc_player_status *st);

/* Copy the presented frame as BGRA into `bgra` (`cap` bytes) if it is newer
 * than `*seq`: 1 copied, 0 nothing new, -1 `cap` too small (`*w`, `*h` set). */
int oc_player_frame(oc_player *p, uint8_t *bgra, size_t cap, int *w, int *h, uint64_t *seq);

void oc_player_close(oc_player *p);

/* Tests: sleep this long after every video decode, to make the decoder slow. */
void oc_player_test_decode_delay(oc_player *p, int ms);

#endif
