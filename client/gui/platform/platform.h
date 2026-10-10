/* OpenChime GUI -- the platform contract (ARCH-80, docs/CLIENT.md §1).
 *
 * The application layer (client/gui/app/) is one portable program: SDL3 for
 * the window, input, timers, clipboard text and cursors; oc_gfx for drawing;
 * sdltext for text. What a platform must still supply itself is listed here,
 * and nothing else in the application names a platform API. Each directory
 * under client/gui/platform/ implements every function in this header:
 *
 *   win32/   Windows: the reference implementation
 *   web/     the browser, under Emscripten
 *   linux/   (next) FreeType/fontconfig text, AT-SPI, libsecret, portals
 *   mac/     (next) Core Text, NSAccessibility, the Keychain
 *
 * A function a platform cannot provide answers "not available" (0, NULL, an
 * empty string) and the application carries on without it; that is the rule,
 * so a new platform starts from stubs and earns each capability.
 *
 * Everything here runs on the main thread unless it says otherwise. What the
 * platform has to tell the application asynchronously -- a URL handed over by
 * a second launch, a tray click, an accessibility invoke -- arrives as an SDL
 * user event of type oc_plat_event_type() with one of the OC_PLAT_EV_* codes,
 * so the application has exactly one inbox. */
#ifndef OC_PLATFORM_H
#define OC_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include "sdltext.h"

/* ---- the process ----------------------------------------------------------- */

/* The application's part of a crash report: it appends its state and the
 * breadcrumbs. Runs inside the platform's crash handler, so it must not
 * allocate. */
typedef void (*oc_plat_report_fn)(FILE *report, void *user);

/* First thing in main(), before SDL: crash handlers, COM, anything a process
 * declares once. */
void  oc_plat_boot(void);
/* After SDL_Init: re-assert whatever SDL replaced (its own crash filter). */
void  oc_plat_after_sdl_init(void);
/* Where crash reports go and what the application adds to one. `dir` is copied. */
void  oc_plat_crash_reports(const char *dir, oc_plat_report_fn app_part, void *user);
/* The per-user data directory for this application, created if needed.
 * 0 when the platform has none (then the caller uses the current directory). */
int   oc_plat_data_dir(char *out, size_t cap);
/* Memory backed by the file at `path`, `size` bytes, zero-filled when new, so
 * what is written there survives the process however it dies. NULL when the
 * platform cannot (then the application keeps a plain buffer). */
void *oc_plat_ring_map(const char *path, size_t size);
void  oc_plat_ring_unmap(void *p, size_t size);
/* Deliver `payload` of `kind` ("url", "action") to the instance of this
 * application already running. 1 when one took it: this process should exit. */
int   oc_plat_handoff(const char *kind, const char *payload);
/* A browser sign-in's start and its result (AUTH.md §8.10, WEB.md). The
 * application sends the person to `url` to sign in: a desktop opens the
 * default browser and keeps running; a page IS the browser and goes there
 * itself, to be served again with the result. 1 when the platform did. */
int   oc_plat_open_signin(const char *url);
/* The result this process was started with, if any: the token the sign-in
 * page sent back (`?token=` on a page's address, taken and stripped), into
 * `out`. 1 when there was one. A desktop has none: its listener hears it. */
int   oc_plat_signin_result(char *out, size_t cap);
/* Last thing before exit. */
void  oc_plat_quit(void);
const char *oc_plat_name(void);

/* ---- events the platform raises ------------------------------------------- */

enum {
    OC_PLAT_EV_HANDOFF = 1,     /* data1: malloc'd "kind\npayload"; the application frees it */
    OC_PLAT_EV_A11Y_INVOKE,     /* data1/data2: the high and low halves of the token */
    OC_PLAT_EV_TASKBAR_RESET,   /* the shell restarted: the badge is gone, apply it again */
    OC_PLAT_EV_TRAY             /* code2 in data1: OC_TRAY_* */
};
enum { OC_TRAY_CLICK = 1, OC_TRAY_RCLICK, OC_TRAY_BALLOON_CLICK };
uint32_t oc_plat_event_type(void);

/* ---- the main window ------------------------------------------------------ */

/* Once, after the window exists: the application's identity for the shell,
 * its icon, file drops, and the hooks the platform needs on the window. */
void  oc_plat_window_attach(SDL_Window *w);
/* Bring the window forward and give it the keyboard, however the platform
 * lets a process that does not own the foreground do that. */
void  oc_plat_window_front(SDL_Window *w);
/* Whether the platform says this window is the foreground one (the dump reports it). */
int   oc_plat_window_is_front(SDL_Window *w);
/* Whether this window holds the keyboard in the platform's own accounting: a
 * process can give its window the keyboard without owning the foreground. */
int   oc_plat_window_has_keyboard(SDL_Window *w);
/* The caption follows the theme (what the application does not paint). */
void  oc_plat_window_caption_dark(SDL_Window *w, int dark);
/* Round a popup's corners the platform's way. 0 when honoured, else the
 * platform's error code (the test dump reports it). */
int   oc_plat_window_round(SDL_Window *w);
/* Keep a window out of screen captures and recordings. 1 when honoured. */
int   oc_plat_window_capture_exclude(SDL_Window *w, int on);
/* Clicks pass through the window to what is under it. */
void  oc_plat_window_click_through(SDL_Window *w, int on);
/* The taskbar/dock badge: 0 clears, -1 a plain dot, n the count. 1 when applied. */
int   oc_plat_badge(SDL_Window *w, int count, uint32_t rgb);   /* rgb: the disc's colour */
int   oc_plat_badge_status(void);          /* the last failure's code, for the dump */
/* Upload progress on the taskbar button: total 0 clears. */
void  oc_plat_progress(SDL_Window *w, uint64_t done, uint64_t total);
/* The rectangle on screen of a share source ("screen:N", "window:HEX"), in
 * the desktop's pixel coordinates. 0 when it cannot be found. */
int   oc_plat_share_rect(const char *source_id, int *x, int *y, int *w, int *h);

/* ---- the tray ------------------------------------------------------------- */

int   oc_plat_tray_init(SDL_Window *w, const char *tooltip);   /* 1 when there is one */
void  oc_plat_tray_done(void);
/* The platform's notification backend may ride on the tray icon (a balloon).
 * What osnotify's init takes as `window` and `tray_id`; NULL when no tray. */
void *oc_plat_tray_notify_handle(unsigned *tray_id);

/* ---- files, the shell, settings ------------------------------------------- */

/* A file picker. `filter` is "Label|*.png;*.jpg" or NULL for any file. The
 * paths picked are written NUL-separated and double-NUL-terminated; returns how
 * many, 0 when cancelled or when the platform has no picker. */
int   oc_plat_pick_files(SDL_Window *owner, const char *filter, int multi, char *out, size_t cap);
/* A save-as picker; `suggested` is the file name offered. 1 with the path in `out`. */
int   oc_plat_pick_save(SDL_Window *owner, const char *suggested, char *out, size_t cap);
/* The application has finished writing the file a save-as picker named: a
 * platform that saves by other means (a browser's download) acts now. */
void  oc_plat_file_saved(const char *path);
/* The certificate viewer the platform's administrators know. 0 when there is none. */
int   oc_plat_view_certificate(SDL_Window *owner, const uint8_t *der, size_t len);
/* Start with the desktop session. */
int   oc_plat_autostart_get(void);
void  oc_plat_autostart_set(int on);
/* Register this executable for `scheme://` links, for this user. */
void  oc_plat_url_scheme_register(const char *scheme);
/* Play one of the platform's own notification sounds by its name. */
void  oc_plat_sound(const char *name);
/* Milliseconds since the last keyboard or pointer input anywhere; 0 if unknown. */
uint32_t oc_plat_idle_ms(void);

/* ---- the clipboard beyond text (text is SDL's) ---------------------------- */

/* Files on the clipboard: malloc'd, NUL-separated, double-NUL-terminated; NULL none. */
char    *oc_plat_clipboard_files(void);
/* An image on the clipboard as a file's bytes (PNG or GIF), malloc'd, with its
 * extension in `ext`; NULL when there is none. */
uint8_t *oc_plat_clipboard_image(size_t *len, char ext[8]);

/* ---- images ---------------------------------------------------------------- */

/* Decode an image file's bytes to premultiplied BGRA, stride w*4, malloc'd.
 * NULL when it cannot. */
uint8_t *oc_plat_image_decode(const uint8_t *bytes, size_t len, int *w, int *h);

/* ---- text ------------------------------------------------------------------ */

/* The sdltext backend for this platform (DirectWrite, the canvas, ...). */
st_ctx *oc_plat_text_create(const st_sink *sink);

#endif /* OC_PLATFORM_H */
