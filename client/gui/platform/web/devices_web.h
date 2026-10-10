/* OpenChime web client -- the browser's device list, for the capture and audio
 * backends (cap_web.c, audio_web.c). enumerateDevices is a promise on the
 * page's thread; the application asks for devices from any thread (a
 * recorder's, a call's), so the page keeps the list in a cache that any
 * thread copies out synchronously, refreshed when the browser says the
 * devices changed and after every successful open (labels appear once a
 * device has been allowed). On the page's thread with an empty cache the
 * first ask waits for the browser's answer, so a picker is never empty on its
 * first opening. */
#ifndef OC_DEVICES_WEB_H
#define OC_DEVICES_WEB_H

#include <stddef.h>

/* Copy the devices of `kind` ("videoinput", "audioinput", "audiooutput") into
 * records of `stride` bytes at `out`, at most `cap`: the id (NUL-terminated,
 * up to `id_len`) at offset 0, the name at `name_off` (up to `name_len`), and
 * at `def_off` an int, 1 for the default device, when `def_off` >= 0.
 * Returns the count. */
int  oc_webdev_list(const char *kind, char *out, int cap, int stride, int id_len, int name_off, int name_len, int def_off);
/* Ask the browser again (after an open: the labels are there now). */
void oc_webdev_refresh(void);

#endif
