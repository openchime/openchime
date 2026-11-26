/* The Linux capture backends' seams (cap_portal.c): the desktop portal's
 * requests, and the conversion of a PipeWire frame to oc_frame. Declared apart
 * from oc_capture.h because only the Linux GUI build has them (it defines
 * OC_HAVE_PORTAL_CAPTURE), and apart from the backends because a test drives
 * each on its own: the requests against a stand-in portal on a private session
 * bus, and the conversion with buffers it builds itself. Nothing here needs a
 * PipeWire or GLib header to be included. */
#ifndef OC_CAP_PORTAL_H
#define OC_CAP_PORTAL_H

#include <stdint.h>

#include "oc_capture.h"

/* A connection to the session bus for the portal's requests, private to its
 * capture so that its signals reach no one else. `on_closed` is called, on
 * GDBus's worker thread, when the screen cast's session is closed by the
 * portal or the portal leaves the bus; never after oc_cap_portal_close has
 * returned. NULL when there is no session bus. */
typedef struct oc_cap_portal oc_cap_portal;
oc_cap_portal *oc_cap_portal_connect(void (*on_closed)(void *ud), void *ud);
/* The Camera portal: AccessCamera, then OpenPipeWireRemote. OC_CAP_OK with
 * `*fd` a PipeWire remote the caller owns; OC_CAP_DENIED when the person
 * refused or dismissed the request; OC_CAP_FAILED when there is no portal or
 * it failed (oc_capture_detail says which). */
int  oc_cap_portal_camera(oc_cap_portal *p, int *fd);
/* The ScreenCast portal: CreateSession, SelectSources (a screen or a window,
 * one of them, with the cursor drawn in where the portal can), Start, then
 * OpenPipeWireRemote. OC_CAP_OK with `*fd` the remote and `*node` the stream's
 * node on it; the session lasts until oc_cap_portal_close. Errors as for the
 * camera. */
int  oc_cap_portal_screen(oc_cap_portal *p, int *fd, uint32_t *node);
/* Closes the screen cast's session, if there is one, and the connection. */
void oc_cap_portal_close(oc_cap_portal *p);

/* The raw video formats negotiated, as SPA_VIDEO_FORMAT_* values: the packed
 * RGB ones a compositor gives a screen in (BGRx, BGRA, RGBx, RGBA), and for a
 * camera YUY2, NV12 and I420 as well. Writes at most `cap` and returns how
 * many there are. */
int  oc_cap_portal_formats(uint32_t *out, int cap);
/* One frame in `format` (SPA_VIDEO_FORMAT_*) to I420, BT.709 limited range,
 * dst->width × dst->height of it from the top left. `plane` and `stride` are
 * the source's planes: one for the packed formats, Y then interleaved CbCr
 * for NV12, Y, Cb, Cr for I420. Returns 0, or -1 for a format not among
 * oc_cap_portal_formats. */
int  oc_cap_portal_convert(oc_frame *dst, uint32_t format, const uint8_t *const plane[3], const int stride[3]);

#endif
