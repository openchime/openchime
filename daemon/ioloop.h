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
 *                         frame), HTTP_REQ (an HTTP connection's request, parsed,
 *                         or the status it is refused with),
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

#include "protocol.h"
#include "proxyproto.h"
#include "tls.h"

typedef struct oc_ioloop oc_ioloop;

typedef enum { OC_IO_SOURCE, OC_IO_OPENED, OC_IO_FRAME, OC_IO_HTTP_REQ, OC_IO_WRITTEN, OC_IO_CLOSED } oc_io_kind;

/* The most an HTTP connection may send (ARCH-32): a full body plus generous
 * header headroom. Beyond this the request is refused (413). */
#define OC_HTTP_MAX_REQUEST (OC_MAX_BODY_SIZE + 16384u)

typedef struct oc_io_event {
    oc_io_kind kind;
    uint64_t   conn_id;
    int        fd;
    int        http;           /* OPENED: did not negotiate oc/1 (ARCH-54) */
    char       source[46];     /* SOURCE / OPENED: the peer, as a PROXY v2 header named it if trusted */
    uint64_t   written;        /* WRITTEN: bytes written to the socket since it opened.
                                * The loop counts what it sent; the difference is
                                * what is waiting, and no report can be stale. */
    uint8_t   *data;           /* FRAME: the frame. HTTP_REQ: method, path and body, in turn */
    size_t     len;
    /* HTTP_REQ: 0 for a request, whose parts are the lengths below and whose body
     * is JSON when is_json; or the status it is refused with (400, 413). Only one
     * is reported per connection: what follows it is not read. */
    int        http_status;
    size_t     method_len, path_len, body_len;
    int        is_json;
    struct oc_io_event *next;
} oc_io_event;

/* `nthreads` threads terminating TLS with `tls`, whose configuration they share
 * (it is read-only, and its random generator is mbedTLS's thread-safe one).
 * `trusted` names the forwarders whose PROXY v2 header is read; it must outlive
 * the pool. NULL on failure. */
oc_ioloop *oc_ioloop_start(int nthreads, oc_tls_server *tls, const oc_trusted_proxies *trusted);
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

/* The soft level: WRITTEN is reported each time a connection's waiting output
 * falls to or below it from above, and whenever it empties. */
#define OC_IO_SOFT (256u * 1024u)

#endif /* OPENCHIME_IOLOOP_H */
