/*
 * The daemon's I/O threads (ARCH-22): everything a connection does on the wire,
 * so the event loop sees only plaintext.
 *
 * The event loop accepts a connection and hands its socket here. From then on
 * one I/O thread owns it for its life -- the PROXY v2 header, the TLS handshake,
 * decryption, reassembly into frames, encryption and writing -- and talks to the
 * event loop through two queues:
 *
 *   events, I/O -> loop   SOURCE (a forwarder's PROXY v2 header named the
 *                         client; the thread waits for PROCEED or CLOSE before
 *                         spending a handshake on it), OPENED (handshake done: the peer, and whether it
 *                         negotiated oc/1), FRAME (one whole binary-protocol
 *                         frame), HTTP_REQ (an HTTP connection's request for a
 *                         route the loop answers, parsed),
 *                         WRITTEN (how much has been written so far),
 *                         CLOSED (the connection is finished).
 *   commands, loop -> I/O PROCEED (after SOURCE), SEND (bytes to write, optionally closing after),
 *                         PAUSE / RESUME reading, CLOSE.
 *
 * A connection belongs to thread conn_id % nthreads, so each of its mbedTLS
 * contexts is only ever touched by one thread, as mbedTLS requires, and the one
 * FIFO each way keeps its frames in the order they arrived and its output in
 * the order it was sent.
 *
 * HTTP (ARCH-32) is parsed and routed here, against the site of the listener
 * the connection came from (http.h): the TLS port's for a peer that did not
 * negotiate oc/1, the health port's for a plaintext connection (ARCH-25). A
 * route that touches no state, and every refusal -- 400, 404, 405, 408, 413 --
 * is answered on the I/O thread; only a request for an OC_HTTP_LOOP route is
 * reported. A request that has not arrived whole within the request timeout is
 * answered 408. One request per connection: what follows it is not read.
 *
 * The socket is closed only when the loop says so (CLOSE), never by the I/O
 * thread alone: the loop indexes connections by descriptor, and a descriptor
 * closed underneath it could be handed to the next connection it accepts while
 * it still held the old one. An I/O thread that finds a connection finished
 * reports CLOSED and waits.
 */

#ifndef OPENCHIME_IOLOOP_H
#define OPENCHIME_IOLOOP_H

#include <stddef.h>
#include <stdint.h>

#include "http.h"
#include "protocol.h"
#include "proxyproto.h"
#include "tls.h"

typedef struct oc_ioloop oc_ioloop;

typedef enum { OC_IO_SOURCE, OC_IO_OPENED, OC_IO_FRAME, OC_IO_HTTP_REQ, OC_IO_WRITTEN, OC_IO_CLOSED } oc_io_kind;

/* How long an HTTP connection has to send its whole request, from the moment
 * it can (the handshake done, or the plaintext socket adopted). */
#define OC_HTTP_REQUEST_TIMEOUT_MS 10000u

typedef struct oc_io_event {
    oc_io_kind kind;
    uint64_t   conn_id;
    int        fd;
    int        http;           /* OPENED: did not negotiate oc/1 (ARCH-54), or plaintext */
    char       source[46];     /* SOURCE / OPENED: the peer, as a PROXY v2 header named it if trusted */
    uint64_t   written;        /* WRITTEN: bytes written to the socket since it opened.
                                * The loop counts what it sent; the difference is
                                * what is waiting, and no report can be stale. */
    uint8_t   *data;           /* FRAME: the frame. HTTP_REQ: method, path and body, in turn */
    size_t     len;
    /* HTTP_REQ: the parts' lengths, and whether the body is JSON. Only one is
     * reported per connection: what follows it is not read. */
    size_t     method_len, path_len, body_len;
    int        is_json;
    /* ...and, after the body, the Host and Origin headers (the sign-in pages'
     * same-origin check), and whether the body is a form. */
    size_t     host_len, origin_len;
    int        is_form;
    struct oc_io_event *next;
} oc_io_event;

/* `nthreads` threads terminating TLS with `tls`, whose configuration they share
 * (it is read-only, and its random generator is mbedTLS's thread-safe one).
 * `trusted` names the forwarders whose PROXY v2 header is read. `tls_site` is
 * what an HTTP peer on the TLS port is served, `plain_site` what a plaintext
 * connection is (either NULL: 404 for everything). All three must outlive the
 * pool. NULL on failure. */
oc_ioloop *oc_ioloop_start(int nthreads, oc_tls_server *tls, const oc_trusted_proxies *trusted,
                           const oc_http_site *tls_site, const oc_http_site *plain_site);
/* Stop the threads and close every socket they still hold. */
void oc_ioloop_stop(oc_ioloop *io);

/* Readable when events are waiting. */
int          oc_ioloop_eventfd(const oc_ioloop *io);
oc_io_event *oc_ioloop_next(oc_ioloop *io);
void         oc_io_event_free(oc_io_event *e);

/* Hand an accepted, non-blocking socket over. `via_proxy`: it came from a trusted
 * forwarder and begins with a PROXY v2 header. -1 if the command could not be
 * queued (the caller closes the socket). */
int  oc_ioloop_adopt(oc_ioloop *io, int fd, uint64_t conn_id, const char *source, int via_proxy);
/* Hand over a plaintext HTTP socket (the health port, ARCH-25): no TLS and no
 * PROXY header; it is OPENED at once, as HTTP, and served `plain_site`. */
int  oc_ioloop_adopt_plain(oc_ioloop *io, int fd, uint64_t conn_id, const char *source);
/* Write `len` bytes (copied), in order after everything sent before; with
 * `close_after`, finish the connection once they are written. */
int  oc_ioloop_send(oc_ioloop *io, uint64_t conn_id, int fd, const uint8_t *buf, size_t len,
                    int close_after);
void oc_ioloop_pause(oc_ioloop *io, uint64_t conn_id, int fd, int paused);
/* After SOURCE: go on to the handshake. */
void oc_ioloop_proceed(oc_ioloop *io, uint64_t conn_id, int fd);
/* Write what is queued if the socket takes it now, then close the socket. The
 * connection's last message is this; nothing more is reported for it. */
void oc_ioloop_close(oc_ioloop *io, uint64_t conn_id, int fd);

/* The request timeout, OC_HTTP_REQUEST_TIMEOUT_MS unless set (0 restores it); a
 * test's knob, so a stalled client can be seen without waiting ten seconds.
 * Any thread; process-wide. */
void oc_ioloop_set_http_timeout_ms(uint64_t ms);

/* The soft level: WRITTEN is reported each time a connection's waiting output
 * falls to or below it from above, and whenever it empties. */
#define OC_IO_SOFT (256u * 1024u)

#endif /* OPENCHIME_IOLOOP_H */
