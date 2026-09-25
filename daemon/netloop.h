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

#include "dbwriter.h"
#include "tls.h"

/* Serve the binary protocol on `port` (TCP, all interfaces), terminating TLS
 * with `tls` and routing DB work through `dbw`. Blocks until *stop becomes
 * non-zero (checked between epoll cycles). Returns 0 on clean shutdown, -1 on a
 * fatal setup error. */
int oc_netloop_run(int port, oc_tls_server *tls, oc_dbwriter *dbw,
                   volatile sig_atomic_t *stop);

/* Wire the audio relay sidecar (REQ-150, ARCH-31): the IPC socket to it and the
 * UDP port it listens on. Call before oc_netloop_run. With ipc_fd < 0 (the
 * default) calls still form but carry no media endpoint. */
void oc_netloop_set_audio(int ipc_fd, uint16_t udp_port);

/* How to bring the audio sidecar back when it exits: `respawn` starts a new one
 * on the same UDP port and returns the net loop's end of its IPC socket, or -1.
 * The net loop notices the exit (EOF on the IPC socket), restarts it, and
 * re-authorizes everyone already in a call. If it dies again within seconds of
 * starting, repeatedly, or `respawn` fails, calls are refused from then on
 * rather than handed a dead port. NULL (the default): an exited sidecar is not
 * restarted, and calls are refused. Call before oc_netloop_run. */
void oc_netloop_set_audio_respawn(int (*respawn)(void *ctx), void *ctx);

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

#endif /* OPENCHIME_NETLOOP_H */
