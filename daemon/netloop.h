/*
 * OpenChime network event loop (ARCH-22).
 *
 * A single-threaded epoll loop multiplexes all client connections with
 * non-blocking I/O — not thread-per-connection. It owns the listening socket
 * and, per connection, the TLS session (src/tls.c) and the frame reassembler
 * (src/framebuf.c). Accepted writes (AUTH, SEND) are handed to the DB-writer
 * thread (ARCH-5) as jobs; results come back through the writer's eventfd,
 * which this loop polls, and are delivered to the right connections here — so
 * all socket I/O stays on this thread and all DB writes stay on the writer.
 */

#ifndef OPENCHIME_NETLOOP_H
#define OPENCHIME_NETLOOP_H

#include <signal.h>
#include <stdint.h>

#include "dbwriter.h"
#include "tls.h"

/* Serve the binary protocol on `port` (TCP, all interfaces), terminating TLS
 * with `tls` and routing DB work through `dbw`. Blocks until *stop becomes
 * non-zero (checked between epoll cycles). Returns 0 on clean shutdown, -1 on a
 * fatal setup error. */
int oc_netloop_run(int port, oc_tls_server *tls, oc_dbwriter *dbw,
                   volatile sig_atomic_t *stop);

/* Serve the plaintext health port (ARCH-25) beside the TLS port: `/healthz`
 * and the landing page, through the same HTTP stack. -1 (the default) serves
 * none. Call before oc_netloop_run. */
void oc_netloop_set_health_port(int port);

/* Wire call media (REQ-150, ARCH-18/73): the bound UDP socket the loop's relay
 * runs on (relay.h), and the port it is bound to. The caller keeps the socket.
 * A loop takes it when it starts and gives it back when it stops, so set it
 * before oc_netloop_run; a second loop started meanwhile finds none. With
 * udp_fd < 0 (the default) there is no media endpoint and calls are refused. */
void oc_netloop_set_audio(int udp_fd, uint16_t udp_port);

/* Drop a call participant after `ms` without a packet rather than
 * OC_AUDIO_SILENCE_MS (0 restores it); a test's knob, so a sweep can be seen
 * without waiting twenty seconds. Any thread; applied on the loop's next tick. */
void oc_netloop_set_relay_silence_ms(uint64_t ms);

/* Hold a lost connection's seat in a call for `ms` rather than
 * OC_CALL_REJOIN_GRACE_MS (0 restores it); a test's knob, so a seat's expiry can
 * be seen without waiting fifteen seconds. Any thread. */
void oc_netloop_set_call_grace_ms(uint64_t ms);

/* Hold a connection's presence changes to OC_PRESENCE_RATE_MAX per `ms` rather
 * than per ten seconds (0 restores it); a test's knob, so the deferred last word
 * can be seen without waiting out the window. Any thread. */
void oc_netloop_set_presence_rate_ms(uint32_t ms);

/* How long a device code lives (AUTH.md §8.11); 0 restores ten minutes. A
 * test's knob, read when a loop starts. */
void oc_netloop_set_device_ttl_ms(uint64_t ms);
/* ...and the interval, in seconds, a client is asked to poll at; 0 restores five. */
void oc_netloop_set_device_interval_s(unsigned s);

/* Wire the outbound push emitter (ARCH-85). When set, a committed SEND fans a
 * contentless notify decision to it for offline mobile delivery. NULL (the
 * default) disables push. May be called while the loop runs: a managed box
 * starts push once it has claimed its binding. */
struct oc_push;
void oc_netloop_set_push(struct oc_push *push);

/* Wire the invitation mail report (invite_mail.h). When set, a committed invite
 * bound to an address is reported to central for its mail. NULL (the default)
 * reports nothing. May be called while the loop runs. */
struct oc_invite_mail;
void oc_netloop_set_invite_mail(struct oc_invite_mail *m);

/* Called once, on the loop's thread, when the listener is bound and taking
 * connections and the loop is about to serve: the moment a workspace is up. It
 * must return promptly -- anything slow belongs on a thread it starts. NULL (the
 * default) calls nothing. Call before oc_netloop_run. */
void oc_netloop_set_ready(void (*ready)(void *ctx), void *ctx);

/* Wire the link-unfurl worker (REQ-222, ARCH-105). When set, a committed SEND
 * or EDIT has its URLs extracted and queued for fetching, and a stored unfurl
 * fans an UNFURL frame to the channel. Call before oc_netloop_run; NULL (the
 * default, and the OPENCHIME_UNFURL=off state) disables unfurls. */
struct oc_unfurler;
void oc_netloop_set_unfurler(struct oc_unfurler *u);

/* Wire read-aloud's synthesis engine (REQ-291-295, ARCH-111). The daemon passes
 * the voice model built into it; a test passes a stub. Call before
 * oc_netloop_run; NULL (the default, and a daemon built with TTS=0) means the
 * daemon advertises no read-aloud and answers AUDIO_GET with TTS_UNAVAILABLE. */
struct oc_tts_engine;
void oc_netloop_set_tts(const struct oc_tts_engine *engine);

/* Wire voice input's recognizer (REQ-296-300, ARCH-112). The daemon passes the
 * model built into it; a test passes a stub. Call before oc_netloop_run; NULL
 * (the default, and a daemon built with STT=0) means the daemon advertises no
 * voice input and refuses STT_BEGIN with STT_UNAVAILABLE. */
struct oc_stt_engine;
void oc_netloop_set_stt(const struct oc_stt_engine *engine);

/* What the loop's turns cost, for the load harness and the tests that hold the
 * loop to a bound. A turn is the work between one epoll_wait returning and the
 * next being called; waiting is not counted. Durations go into a histogram of
 * OC_NETLOOP_HIST_BUCKETS buckets, four to each doubling of microseconds, so a
 * percentile is known to within a quarter-octave. Counted by the loop with
 * relaxed atomics and read from any thread; a snapshot taken while the loop runs
 * may be a turn out of step between fields, which no reader relies on. */
#define OC_NETLOOP_HIST_BUCKETS 96
typedef struct {
    uint64_t turns;
    uint64_t turn_max_us;
    uint64_t turn_hist[OC_NETLOOP_HIST_BUCKETS];
    uint64_t results;      /* database results delivered */
    uint64_t bytes_read;   /* plaintext read from clients */
    uint64_t turn_read_max;/* the most plaintext read in any one turn */
    uint64_t live_visits;  /* connections examined by walks of every connection
                            * that something arriving caused -- a fan-out, a
                            * snapshot -- and not by the sweeps every turn makes */
} oc_netloop_stats;

void oc_netloop_stats_get(oc_netloop_stats *out);
/* For the I/O threads (ioloop.h): `bytes` of plaintext read from one connection
 * in one turn of its thread. */
void oc_netloop_stats_note_read(uint64_t bytes, uint64_t conn_id);
void oc_netloop_stats_reset(void);
/* The turn duration, in microseconds, at or below which `pct` percent of the
 * snapshot's turns fall: the upper edge of the bucket that holds it. */
uint64_t oc_netloop_stats_pct_us(const oc_netloop_stats *s, double pct);

#endif /* OPENCHIME_NETLOOP_H */
