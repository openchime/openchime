# OpenChime — Video messages

How a recorded video message works: what a user can do, how the client captures,
encodes and writes it, how it travels to the daemon as an attachment, how another
client plays it back, and what the tests prove. This is the authoritative design;
it is cross-referenced from ARCHITECTURE.md (ARCH-110), REQUIREMENTS.md (§6.3,
REQ-162–166), PROTOCOL.md (§5.14), SCHEMA.md (migration 0041) and
[AUDIO.md](./AUDIO.md), whose device layer it shares.

A video message is **a file, not a stream.** Nothing here involves a call, the
audio relay or any real-time path; screenshare is a different design
([VIDEO.md](./VIDEO.md)). Camera video *calling* stays excluded (REQ-160), and so
does an audio-only clip (REQ-273).

---

## 1. What a user can do

- **Record** from the composer's camera button. A recording card shows a live
  preview, a camera and a microphone picker, and a level meter. Record counts down
  3-2-1, shows the elapsed time against the cap, and stops on its own at **5:00**.
- **Review** before sending: play it back, retake, discard, or send. The composer's
  text becomes the caption. **Nothing leaves the machine until Send.**
- **Post** into a channel, a DM, or a thread (as a reply).
- **Watch** from a card in the conversation — poster, play mark, duration — that
  opens a player: play and pause, seek, volume, mute, download, close.
- **Find** it: previews say "🎥 Video message (m:ss)"; the Files view lists it as
  video; search matches it with `has:video`.

The text-only client shows `[video message m:ss]` and the download action; it never
plays media (ARCH-75).

## 2. Format

| | |
|---|---|
| Container | MP4 (ISO BMFF), progressive, `moov` before `mdat` |
| Video | VP9 (libvpx) at the quality the user picks — 360p, 720p (the default) or 1080p — at 30 fps; a camera that cannot deliver the size gives the nearest it has, and anything larger is scaled down. One size smaller (720p, or 360p at 24 fps) when the encoder falls behind |
| Video rate | constant-bitrate target 4 Mbps at 1080p, 2.5 Mbps at 720p, 1.2 Mbps at 480p, 0.8 Mbps at 360p; a keyframe every 2 s |
| Audio | Opus (libopus), 48 kHz mono, 20 ms frames, 48 kbps, VOIP application |
| Poster | JPEG, the first keyframe at or after 1 s, at the video's own dimensions |
| Length cap | 5 minutes, stopped by the recorder |
| Size cap | `OPENCHIME_MAX_VIDEO_MESSAGE_SIZE`, 160 MiB by default, enforced by the daemon; the recorder stops at 155 MiB so a take always fits the default |
| File name | `<display name>-video-<YYYYMMDDHHMMSS>.mp4`, and the poster the same with `.jpg` |

Five minutes is about 30 MB at 360p, 95 MB at 720p and 150 MB at 1080p. The daemon
enforces bytes, not seconds (§6).

## 3. Capture

### 3.1 The interface

One interface for every platform, in `client/core/media/oc_capture.h`:

```c
typedef struct {                      /* oc_media.h */
    int      width, height;          /* even */
    int64_t  pts_us;                  /* oc_media_clock_us() at capture */
    uint8_t *plane[3];                /* Y, U, V */
    int      stride[3];
} oc_frame;                           /* planar I420, BT.709 limited range */

int  oc_capture_list(oc_capture_device *out, int cap);
oc_capture *oc_capture_open(const char *device_id, int want_w, int want_h, int want_fps, int *err);
int  oc_capture_start(oc_capture *c);
int  oc_capture_next(oc_capture *c, oc_frame *f, int timeout_ms);  /* 1 frame, 0 timeout, <0 error */
void oc_capture_stop(oc_capture *c);
void oc_capture_close(oc_capture *c);
```

A backend is an `oc_capture_backend` — the same six operations — and the front
functions forward to the one in use: the synthetic source when
`OPENCHIME_TEST_CAPTURE` asks for it, otherwise `oc_capture_platform()`, which is
NULL on a platform whose backend is not built yet.

Errors are typed so the UI can say the right thing: `OC_CAP_DENIED` (the operating
system blocks the camera), `OC_CAP_NODEVICE`, `OC_CAP_BUSY` (another application
holds it), `OC_CAP_FAILED`.

**The frame format is fixed here, before any backend.** Cameras deliver NV12, YUY2,
biplanar YUV, BGRA or MJPEG depending on the platform and the device. Every backend
converts to planar I420 so the encoder — whose own input is I420 — and the preview
see exactly one thing. Retrofitting a common format across six backends later would
be expensive; choosing it first is free.

**One clock.** Video frames and microphone buffers are stamped from
`oc_media_clock_us()`, a monotonic clock, so the recorder can interleave and sync
them without trusting either device's own timestamps.

### 3.2 Backends

| Platform | Source | Native format | Built |
|---|---|---|---|
| Windows | Media Foundation `IMFSourceReader`, video processing enabled, asynchronous | NV12 (the reader converts YUY2 and MJPEG) | built (`cap_mf.c`) |
| Linux | V4L2, mmap streaming | YUYV, or MJPEG decoded with stb_image | with the Linux client |
| macOS | AVFoundation `AVCaptureVideoDataOutput` | 420YpCbCr8BiPlanar | with the macOS client |
| iOS | AVCaptureSession, the same output plus orientation | 420YpCbCr8BiPlanar | with the iOS client |
| Android | Camera2 through the NDK, `AImageReader` | YUV_420_888 with row and pixel strides | with the Android client |
| Web | `getUserMedia` → `MediaStreamTrackProcessor` → `VideoFrame.copyTo` | I420 | with the web client |
| Tests | a synthetic source: moving bars, a frame counter, a denied mode | I420 | built (`capture.c`) |

**The Windows reader runs asynchronously.** A blocking `ReadSample` on a camera
that stops delivering never returns and cannot be cancelled, so closing the
camera would hang; with a callback, closing stops asking for frames, flushes, and
waits a bounded time for the callback to go quiet.

**Apple platforms use a thin Objective-C file** exposing a flat C header, compiled
with clang beside the C sources, rather than driving the Objective-C runtime through
`objc_msgSend` by hand.

**The synthetic source** is selected with `OPENCHIME_TEST_CAPTURE=synthetic` (or
`denied`, which fails with `OC_CAP_DENIED`). It is how every automated test and the
GUI harness record without a camera.

### 3.3 Microphone and speaker

Audio goes through the **miniaudio** device layer AUDIO.md §3.2 chose for calls,
in `client/core/media/audio_dev.{c,h}`: device lists, capture and playback, and
lock-free single-producer rings between the device callback and the media threads.
The callback never allocates, locks or does I/O. Video messages build this layer
first; the call client reuses it. Recording opens capture at **48 kHz mono**
(calls run at 16 kHz), resampled by miniaudio when the device runs at another rate.

## 4. Recording

`client/core/media/recorder.c` turns a capture device and a microphone into an MP4
and a poster, both **held in memory**: a client writes no files (ARCH-88), and five
minutes of video is at most 155 MB.

- **Threads.** The capture thread pulls frames; the audio callback fills a ring; one
  encode thread takes both, encodes, and hands samples to the muxer. Queues are
  bounded so a slow encoder shows up as queue depth, never as unbounded memory.
- **Falling behind.** When the encode queue holds more than 500 ms, the encoder is
  restarted once a size smaller — 1080p to 720p, 720p to 360p at 24 fps; its first
  frame is a keyframe carrying the new size, which VP9 decoders follow. The track
  and the media row keep the size the recording started at, which has the same
  shape.
- **The byte budget.** The recorder also stops when the samples reach 155 MiB, so a
  take cannot outgrow the daemon's default video cap and fail at upload.
- **The cap.** The recorder stops when the recorded duration reaches the cap. Tests
  shorten it with `OPENCHIME_TEST_VIDEO_CAP_MS`.
- **Poster.** The first keyframe at or after 1 s is decoded back and written as JPEG
  (stb_image_write). A recording shorter than 1 s uses its first keyframe.
- **Nothing on disk.** Encoded samples accumulate in memory while recording, and
  finishing assembles the file in memory. Discarding frees it, sending frees it once
  the upload completes, and a crash takes it with the process — so an interrupted
  recording leaves nothing behind by construction, with no sweep to run (REQ-166).

### 4.1 Encoder settings

| libvpx VP9 | |
|---|---|
| Deadline | `VPX_DL_REALTIME` |
| `VP8E_SET_CPUUSED` | 8 |
| Row multithreading | on; `g_threads` = logical cores − 1 |
| `g_lag_in_frames` | 0 |
| Rate control | CBR, target per §2 |
| `kf_max_dist` | 2 s of frames |
| Content tuning | default (camera), not screen |

| libopus | |
|---|---|
| Application | `OPUS_APPLICATION_VOIP` |
| Rate, channels, frame | 48 000 Hz, 1, 20 ms |
| Bitrate | 48 000, VBR |

## 5. The MP4 profile

`shared/oc_mp4.{c,h}` writes and reads **exactly one profile**, which is
what makes an in-tree implementation reasonable. No small, maintained library covers
VP9 and Opus in MP4, and a general demuxer is far more parser than one fixed layout
needs. It lives in `shared/` because the daemon writes the same profile with the video
`trak` left out, for read-aloud renders (READ-ALOUD.md §4); the reader accepts either.

```
ftyp   major "isom", compatible "isom" "iso6" "mp41"
moov
  mvhd
  trak (video)
    tkhd
    mdia  mdhd (timescale 90000) · hdlr "vide"
      minf  vmhd · dinf/dref
        stbl
          stsd  vp09 (width, height) + vpcC (profile 0, level, bit depth 8, 4:2:0, BT.709)
          stts · stss (keyframes) · stsz · stsc · stco
  trak (audio)
    tkhd
    edts  elst (skips the Opus pre-skip)
    mdia  mdhd (timescale 48000) · hdlr "soun"
      minf  smhd · dinf/dref
        stbl
          stsd  Opus (channels 1, rate 48000) + dOps (PreSkip 312, input rate 48000)
          stts · stsz · stsc · stco
mdat
```

- **Writing.** Samples accumulate in a buffer as they are encoded. Finishing
  assembles `ftyp`, `moov` and `mdat`, with `stco` offsets computed for `moov`
  first; `moov`'s size does not depend on the offsets it holds, so it is measured
  once and then written. VP9 has no reordering, so there is no `ctts`.
- **Reading.** The demuxer accepts this layout and nothing else. Every box size is
  checked against its parent, table entry counts are checked against the file size,
  and anything unexpected is a clean refusal, never a crash. It exposes each track's
  samples with timestamps and the video keyframes for seeking. It is fuzzed in
  `make test` like every other parser the client runs over bytes it did not write.

## 6. Posting and the daemon

A video message is **an ordinary attachment** (REQ-140) with a media row beside it.

### 6.1 The sequence

1. Upload the poster JPEG (ordinary upload frames).
2. Upload the MP4 (ordinary upload frames).
3. `ATTACH_MEDIA_SET {attachment_id, media_kind = 1, duration_ms, width, height,
   poster_id}` → `ATTACH_MEDIA_OK`, or an error.
4. `SEND` (or `SEND_REPLY` in a thread) with the caption and the video attached.

The client runs these as one queued job (§7.1), so a thumbnail fetch in flight does
not refuse the post, and a failure at any step cancels the rest and reports why.

### 6.2 What the daemon checks

`ATTACH_MEDIA_SET` is refused with `MEDIA_INVALID` unless the video attachment was
uploaded by the caller, is finalized and not yet posted, has `mime = video/mp4`,
a `duration_ms` no longer than five minutes, and even dimensions no larger than
1920×1080 — and unless the poster is the same caller's finalized `image/jpeg` in the
same channel, no larger than 1 MiB. A video larger than
`OPENCHIME_MAX_VIDEO_MESSAGE_SIZE` is refused with `MEDIA_TOO_LARGE`.

**The duration is advisory.** The daemon links no codec (ARCH-110), so it cannot
measure one; a modified client could report thirty seconds for a longer file. The
byte cap is the bound it can enforce, and it is set from the five-minute cap at the
bitrates in §2.

### 6.3 The poster belongs to its video

The poster is its own attachment, never itself attached to a message. Left to the
ordinary rules it would be an orphan: collected by the maintenance sweep, and
unreadable by anyone but its uploader. So:

- the **orphan sweep skips** an attachment named as a poster, as it skips avatars
  and custom emoji;
- **age expiry and eviction reclaim the poster with its video**;
- **reading a poster is allowed exactly when reading its video is** — the same
  channel-read check, applied through the media row.

### 6.4 Where the metadata travels

Every attachment entry on the wire carries its media fields — on live messages,
backfill, history, thread replies and forwards — so a client never needs a second
round trip to draw a card. `FILE_ENTRY` carries the kind and duration for the Files
view. Search's `has:video` matches `video/` attachments.

## 7. The client core

### 7.1 Transfers queue

A connection runs one transfer at a time. The client used to refuse a second one as
busy; it now **queues** uploads and downloads in order. A video post is one job made
of the four steps in §6.1, cancellable as a whole. Progress is reported per chunk,
throttled, as bytes done and total — for the upload bar and the download ring.

### 7.2 Downloads of a video

A video message is fetched into memory like any attachment, but under its own
ceiling (the daemon's video cap, not the inline-attachment one) and with progress
reported. The client keeps the most recently played videos, up to 256 MiB, and drops
the oldest past that; nothing is written to disk (ARCH-88). **Download** in the
player is the one place bytes reach a file, and only one the user chose.

## 8. Playback

`client/core/media/player.c` plays an MP4 held in memory.

- **Decode.** A worker thread demuxes and decodes VP9 to I420 and Opus to PCM.
- **Sync.** **The audio clock is the master** — the samples the speaker has
  consumed. A video frame is shown when its presentation time is at or before that
  clock; a frame still unshown when the next is also due is dropped rather than
  shown out of step, though one is always shown at least every 100 ms. A decoder
  that falls further behind skips to the latest keyframe the clock has passed. With
  no audio track or no speaker, the monotonic clock stands in.
- **Seek.** To the keyframe at or before the target. Opus restarts 80 ms early and
  discards up to the matching sample, so sound resumes converged.
- **Output.** I420 converted to BGRA for the UI; PCM into the playback ring. Tests
  use `OPENCHIME_TEST_AUDIO=synthetic`, a sink that consumes at real-time pace.

## 9. The Win32 client

- **Composer:** a camera button beside the attach button, shown when a camera
  exists. The composer's text becomes the caption; with a thread open, the video
  goes into the thread.
- **Recording card:** an overlay with the live preview, dropdowns for the quality
  (360p, 720p, 1080p — kept with the preferences), the camera and the microphone,
  a level meter, the
  3-2-1 countdown, the red recording mark and timer "m:ss / 5:00" with Stop, then
  review — the camera already released — with Play, Retake, Discard and Send.
  Enter records, stops and sends; Esc stops a recording or closes the card.
- **Conversation card:** poster, play mark, duration badge and caption; clicking
  fetches the video with a progress bar, then opens the player. The most recently
  played videos stay in memory, up to 256 MiB.
- **Player:** a full-window overlay — the video fitted to the window, a control bar
  with play and pause, a seek bar, time and volume, mute and download, and close.
  Keys: Space, ← and → by 5 s, ↑ and ↓ for volume, M, Esc.
- **Elsewhere:** a VID badge and a Video filter in the Files view; "🎥 Video
  message (m:ss)" in the sidebar and the notification when there is no caption.
- **Harness:** `gui_drive.sh vm open|record|stop|discard|retake|send|play|mute|close|seek <ms>`,
  and a `vm=` line with the phase, the take, the player and the buttons in `dump`.

## 10. Consent and privacy

- A camera or microphone the operating system blocks is reported as exactly that,
  with a button to the system privacy page (on Windows `ms-settings:privacy-webcam`
  and `ms-settings:privacy-microphone`) — never as a generic failure.
- While recording, a red mark shows on the card and in the window title.
- The camera and microphone are released the moment the recording card closes.
- Nothing is uploaded before Send; a recording never touches the disk, so a
  discarded or interrupted one leaves nothing behind (§4).

## 11. Tests

| Where | What it proves |
|---|---|
| `make test` — media | MP4 write-then-read gives the same samples and timestamps; `ffprobe` reads a recorded file as one `vp9` and one `opus` stream with `moov` first and the right duration; the demuxer survives random and mutated input; VP9 round trip on the synthetic pattern is above 35 dB PSNR and decodes the frames in order; an Opus chirp round-trips aligned to the encoder's look-ahead; a synthetic recording stopped by the cap is the cap's length ± one frame, with its audio and video tracks within a frame and an Opus block of each other, and a JPEG poster from a keyframe at or after 1 s; stopping early and closing mid-recording both finish cleanly; a denied source is reported as denied; the player presents every frame of a normal play, lands seeks on keyframes, and under a slow decoder drops frames and catches up to the next keyframe |
| `make test` — daemon | every `ATTACH_MEDIA_SET` refusal; the poster survives the orphan sweep while its video is live, is collected once the video's message is deleted or the video was never sent, and is reclaimed in the same pass as its video; the media row on SEND, backfill and the Files listing; `has:video`; the media fields on `BROADCAST`, `THREAD_REPLY` and `FILE_ENTRY` and the new frames round-trip; two clients over the wire — one posts, the other receives the fields and fetches the poster |
| `make test` — client core | a video post completes through the queue while a thumbnail fetch is in flight; progress, cancel, a video download above the inline limit, preview text |
| Win32 harness (synthetic camera) | record, review, send; a second client shows the card and plays it; the Files view and previews; the denied path; the cap |
| By hand, once | a real camera and microphone: permission prompts, switching devices, a full five-minute recording, CPU use |
