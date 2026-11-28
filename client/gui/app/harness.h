/*
 * The test harness's line to its driver (docs/details/TESTING.md, "The UI
 * harness"). With `--harness <port>` the client connects to <port> on the
 * driver's address (`--harness-host`, an IPv4 address, 127.0.0.1 unless
 * given), the driver listening there; sends `hello <token>`; and then takes one JSON
 * command per line and answers each with one JSON line, a frame's pixels
 * following as a binary payload of the length the line states.
 *
 * This file is the transport and the JSON reading only. What each command
 * does is app.c's, because it is the app's state it changes.
 */
#ifndef OC_GUI_HARNESS_H
#define OC_GUI_HARNESS_H

#include <stddef.h>
#include <stdint.h>

/* Connect to the driver at `host` (an IPv4 address, NULL for 127.0.0.1) on
 * `port` and introduce ourselves with `token`. 0, or -1 when it cannot be
 * reached. The client dials out: nothing listens in a driven client. */
int  oc_harness_dial(const char *host, int port, const char *token);
/* The next command line, NUL-terminated without its newline, blocking until
 * one arrives. Its length, or -1 when the driver has gone. */
int  oc_harness_read_line(char *buf, size_t cap);
/* One answer line (a newline is added), then `bin_len` bytes of `bin` when
 * there are any. 0, or -1 when the driver has gone. */
int  oc_harness_reply(const char *line, const void *bin, size_t bin_len);
void oc_harness_close(void);

/* A parsed command: its members by name. Values are copied out. */
typedef struct oc_hcmd oc_hcmd;
oc_hcmd *oc_hcmd_parse(const char *json);
void     oc_hcmd_free(oc_hcmd *c);
/* A string member into `out`; 1 when present. */
int      oc_hcmd_str(const oc_hcmd *c, const char *key, char *out, size_t cap);
/* A number member; 1 when present. */
int      oc_hcmd_num(const oc_hcmd *c, const char *key, double *out);

/* `s` as the inside of a JSON string, into `out` (always terminated). */
void     oc_json_escape(const char *s, char *out, size_t cap);

#endif
