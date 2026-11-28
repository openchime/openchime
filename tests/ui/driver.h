/*
 * The UI harness's driver (docs/details/TESTING.md, "The UI harness"): a real
 * daemon in this process, the real desktop client in another, driven over a
 * local connection, one command and one frame at a time.
 *
 * Nothing here waits on a clock. A step ends when its command is answered,
 * and one that needs the daemon ends on `ui_sync`, the daemon's barrier
 * (SYNC). The client's clock moves only by `ui_clock_*`; the daemon's timed
 * work runs only by `ui_daemon_sweep`.
 */
#ifndef OC_TEST_UI_DRIVER_H
#define OC_TEST_UI_DRIVER_H

#include <stddef.h>
#include <stdint.h>

/* The daemon: test hooks on, alice (owner), bob and carol (members), its
 * writer's clock held at `wall_ms`. */
typedef struct ui_daemon ui_daemon;
ui_daemon *ui_daemon_start(const char *db_name, uint64_t wall_ms);
int        ui_daemon_port(const ui_daemon *d);
/* Step the daemon's clock and run its timed sweeps now. */
void       ui_daemon_advance(ui_daemon *d, uint64_t ms);
void       ui_daemon_sweep(ui_daemon *d);
void       ui_daemon_stop(ui_daemon *d);

/* The settings a run chooses; a reference belongs to them. */
typedef struct { int dpi, w, h; const char *tz; const char *theme; int textsize; const char *motion; int density; } ui_settings;
extern const ui_settings UI_DEFAULT;          /* 96 DPI, 1120x820, UTC, dark, text size 1, motion "on" (as designed; "reduced", "system"), density 1 (cozy; 0 compact) */

/* A client: the desktop build (build/openchime.exe), headless unless
 * OC_UI_VISIBLE=1, its clock held at `wall_ms`. NULL when it did not start
 * or did not present `token` (a different one may be asked for, to test a
 * refusal: `present` is what the client is given, `expect` what is accepted). */
typedef struct ui_client ui_client;
ui_client *ui_client_start(const ui_settings *s, uint64_t wall_ms);
ui_client *ui_client_start_tokens(const ui_settings *s, uint64_t wall_ms, const char *present, const char *expect);
void       ui_client_stop(ui_client *c);
/* Whether a client that was refused has gone (its process ended). */
int        ui_client_gone(ui_client *c);

/* One command. 0 when answered ok; `reply` (may be NULL) gets the line. */
int ui_cmd(ui_client *c, const char *json, char *reply, size_t cap);
/* The frame number the last answer named. */
uint64_t ui_frame(const ui_client *c);
int ui_settings_apply(ui_client *c, const ui_settings *s);
int ui_signin(ui_client *c, ui_daemon *d, const char *user, const char *pass);
int ui_sync(ui_client *c);
int ui_clock_advance(ui_client *c, uint64_t ms);
int ui_click_id(ui_client *c, const char *id);
int ui_click_xy(ui_client *c, int x, int y);
int ui_key(ui_client *c, const char *key, int ctrl, int shift);
int ui_text(ui_client *c, const char *s);
/* The accessibility tree as JSON (heap; the frame number left out), and the
 * last frame's pixels (heap RGBA). */
char    *ui_tree(ui_client *c);
uint8_t *ui_shot(ui_client *c, int *w, int *h);
/* Whether the tree names `id`, and its name in `name`. */
int ui_tree_has(ui_client *c, const char *id, char *name, size_t cap);

/* Compare the tree and the frame against tests/ui/ref/<test>/<step>@<tag>,
 * exactly. With OC_UI_UPDATE=1 the references are written instead. 0 equal;
 * -1 different or missing, saying which on stderr and leaving what was seen
 * under build/ui-out/. OC_UI_TRACE=1 prints each command and its answer. */
int ui_check(ui_client *c, const char *test, const char *step, const ui_settings *s);

#endif
