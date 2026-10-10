/* OpenChime GUI -- the browser platform (client/gui/platform/platform.h,
 * docs/WEB.md).
 *
 * What a page can do of the contract, and an honest "not available" for the
 * rest, so the application carries on without it. The page's APIs are
 * asynchronous; the application's calls are not. Asyncify bridges them: an
 * EM_ASYNC_JS call unwinds the application's stack while the promise runs and
 * rewinds it with the answer, so a file picker or an image decode is one
 * ordinary call from the application's side.
 *
 *   images          createImageBitmap + a 2D canvas -> premultiplied BGRA
 *   files           <input type=file> -> the page's filesystem -> a path
 *   saving          a path on the page's filesystem; when the application has
 *                   written it, the browser downloads it (oc_plat_file_saved)
 *   clipboard       navigator.clipboard.read() for an image
 *   the badge       the tab's title
 *   notifications   client/shared/osnotify_web.c
 *   credentials     localStorage (client/gui/platform/web/secret_mem.c)
 *
 * No tray, no second process to hand a URL to (the page's own query is the
 * command line), no crash dump beyond the console, no certificate viewer, no
 * autostart: those are the operating system's, and a page has none. */
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "st_canvas.h"

static uint32_t g_evtype;

uint32_t oc_plat_event_type(void) {
    if (!g_evtype) g_evtype = SDL_RegisterEvents(1);
    return g_evtype;
}

static void push_event(int code, void *d1, void *d2) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = oc_plat_event_type();
    e.user.code = code;
    e.user.data1 = d1;
    e.user.data2 = d2;
    SDL_PushEvent(&e);
}

/* The page hands the application a URL or a notification's click through
 * this (Module.ccall), as another process would on a desktop. */
EMSCRIPTEN_KEEPALIVE void oc_web_handoff(const char *kind, const char *payload) {
    size_t kl = strlen(kind), pl = strlen(payload);
    char *s = malloc(kl + 1 + pl + 1);
    if (!s) return;
    memcpy(s, kind, kl); s[kl] = '\n';
    memcpy(s + kl + 1, payload, pl); s[kl + 1 + pl] = 0;
    push_event(OC_PLAT_EV_HANDOFF, s, NULL);
}

/* ---- the process ----------------------------------------------------------- */

void  oc_plat_boot(void) { }
void  oc_plat_after_sdl_init(void) { }
void  oc_plat_crash_reports(const char *dir, oc_plat_report_fn app_part, void *user) {
    (void)dir; (void)app_part; (void)user;      /* the browser's console is the report */
}
/* The page's filesystem, for what the application writes; persistence itself
 * is localStorage's, behind the credential store. */
int   oc_plat_data_dir(char *out, size_t cap) {
    EM_ASM({ try { FS.mkdir("/data"); } catch (e) {} });
    snprintf(out, cap, "/data");
    return 1;
}
void *oc_plat_ring_map(const char *path, size_t size) { (void)path; (void)size; return NULL; }
void  oc_plat_ring_unmap(void *p, size_t size) { (void)p; (void)size; }
int   oc_plat_handoff(const char *kind, const char *payload) { (void)kind; (void)payload; return 0; }
/* The page goes to the daemon's sign-in page itself; it comes back to /app/
 * with the token (oc_plat_signin_result). */
int   oc_plat_open_signin(const char *url) {
    EM_ASM({ location.assign(UTF8ToString($0)); }, url);
    return 1;
}
/* The token on the page's address, taken once: the address is rewritten
 * without it so a reload or a bookmark does not present it again. */
int   oc_plat_signin_result(char *out, size_t cap) {
    char *t = (char *)EM_ASM_PTR({
        try {
            const u = new URL(location.href);
            const t = u.searchParams.get("token");
            if (!t) return 0;
            u.searchParams.delete("token");
            history.replaceState(null, "", u.toString());
            return stringToNewUTF8(t);
        } catch (e) { return 0; }
    });
    if (!t) return 0;
    snprintf(out, cap, "%s", t);
    memset(t, 0, strlen(t)); free(t);
    return out[0] != 0;
}
void  oc_plat_quit(void) { }
const char *oc_plat_name(void) { return "web"; }

/* ---- the main window ------------------------------------------------------- */

void  oc_plat_window_attach(SDL_Window *w) { (void)w; }
void  oc_plat_window_front(SDL_Window *w) { (void)w; EM_ASM({ window.focus(); }); }
int   oc_plat_window_is_front(SDL_Window *w) { (void)w; return EM_ASM_INT({ return document.hasFocus() ? 1 : 0; }); }
int   oc_plat_window_has_keyboard(SDL_Window *w) { (void)w; return 1; }
void  oc_plat_window_caption_dark(SDL_Window *w, int dark) {
    (void)w;
    EM_ASM({ document.documentElement.style.colorScheme = $0 ? "dark" : "light"; }, dark);
}
int   oc_plat_window_round(SDL_Window *w) { (void)w; return -1; }
int   oc_plat_window_capture_exclude(SDL_Window *w, int on) { (void)w; (void)on; return 0; }
void  oc_plat_window_click_through(SDL_Window *w, int on) { (void)w; (void)on; }

/* The tab's title carries the count, as web applications do. */
int oc_plat_badge(SDL_Window *w, int count, uint32_t rgb) {
    (void)w; (void)rgb;
    char title[64];
    if (count == 0)     snprintf(title, sizeof title, "OpenChime");
    else if (count < 0) snprintf(title, sizeof title, "\xE2\x80\xA2 OpenChime");
    else                snprintf(title, sizeof title, "(%d) OpenChime", count);
    EM_ASM({ document.title = UTF8ToString($0); }, title);
    return 1;
}
int   oc_plat_badge_status(void) { return 0; }
void  oc_plat_progress(SDL_Window *w, uint64_t done, uint64_t total) { (void)w; (void)done; (void)total; }
int   oc_plat_share_rect(const char *id, int *x, int *y, int *w, int *h) {
    (void)id; (void)x; (void)y; (void)w; (void)h; return 0;
}

/* ---- the tray ------------------------------------------------------------- */

int   oc_plat_tray_init(SDL_Window *w, const char *tooltip) { (void)w; (void)tooltip; return 0; }
void  oc_plat_tray_done(void) { }
void *oc_plat_tray_notify_handle(unsigned *tray_id) { if (tray_id) *tray_id = 0; return NULL; }

/* ---- files: the page's filesystem is the application's --------------------- */

/* Open the browser's file picker; copy what was picked into /uploads on the
 * page's filesystem; answer the paths, newline-separated. Empty when
 * cancelled. The picker only opens inside a user gesture, which the click
 * that reached here still is: this runs before the first await. */
EM_ASYNC_JS(char *, web_pick_files, (const char *accept, int multi), {
    try {
        const input = document.createElement("input");
        input.type = "file";
        if (multi) input.multiple = true;
        const acc = UTF8ToString(accept);
        if (acc) input.accept = acc;
        input.style.display = "none";
        document.body.appendChild(input);
        const picked = await new Promise((resolve) => {
            let done = false;
            const finish = (files) => { if (!done) { done = true; resolve(files); } };
            input.addEventListener("change", () => finish(Array.from(input.files || [])));
            input.addEventListener("cancel", () => finish([]));
            /* Browsers without the cancel event: the window regains focus when
             * the dialog closes; give a change event a moment to land first. */
            window.addEventListener("focus", () => setTimeout(() => finish(Array.from(input.files || [])), 600), { once: true });
            input.click();
        });
        input.remove();
        try { FS.mkdir("/uploads"); } catch (e) {}
        const paths = [];
        for (const f of picked) {
            const buf = new Uint8Array(await f.arrayBuffer());
            const path = "/uploads/" + f.name.replace(/[\\/]/g, "_");
            FS.writeFile(path, buf);
            paths.push(path);
        }
        return stringToNewUTF8(paths.join("\n"));
    } catch (e) {
        console.error("pick_files: " + e);
        return 0;
    }
});

/* "Label|*.png;*.jpg" -> ".png,.jpg" */
static void accept_of(const char *filter, char *out, size_t cap) {
    out[0] = 0;
    if (!filter) return;
    const char *bar = strchr(filter, '|');
    if (!bar) return;
    size_t o = 0;
    for (const char *p = bar + 1; *p && o + 8 < cap; ) {
        if (*p == '*') { p++; continue; }
        if (*p == ';') { out[o++] = ','; p++; continue; }
        out[o++] = *p++;
    }
    out[o] = 0;
}

int oc_plat_pick_files(SDL_Window *owner, const char *filter, int multi, char *out, size_t cap) {
    (void)owner;
    char accept[256];
    accept_of(filter, accept, sizeof accept);
    char *list = web_pick_files(accept, multi);
    if (!list) return 0;
    size_t o = 0; int n = 0;
    for (char *p = list; *p; ) {
        char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (o + l + 2 > cap) break;
        memcpy(out + o, p, l); out[o + l] = 0; o += l + 1; n++;
        if (!nl) break;
        p = nl + 1;
    }
    out[o] = 0;
    free(list);
    return n;
}

/* Saving: the application writes to a path on the page's filesystem, and
 * tells the platform when it has (oc_plat_file_saved); the browser then
 * downloads it, which is the only "save as" a page has. */
int oc_plat_pick_save(SDL_Window *owner, const char *suggested, char *out, size_t cap) {
    (void)owner;
    EM_ASM({ try { FS.mkdir("/downloads"); } catch (e) {} });
    char name[256];
    snprintf(name, sizeof name, "%s", suggested && suggested[0] ? suggested : "download");
    for (char *p = name; *p; p++) if (*p == '/' || *p == '\\') *p = '_';
    snprintf(out, cap, "/downloads/%s", name);
    return 1;
}

void oc_plat_file_saved(const char *path) {
    EM_ASM({
        try {
            const path = UTF8ToString($0);
            const data = FS.readFile(path);
            const a = document.createElement("a");
            a.href = URL.createObjectURL(new Blob([data]));
            a.download = path.split("/").pop();
            document.body.appendChild(a); a.click(); a.remove();
            setTimeout(() => URL.revokeObjectURL(a.href), 10000);
            window.ocSaved = (window.ocSaved || 0) + 1;       /* what the test reads */
        } catch (e) { console.error("file_saved: " + e); }
    }, path);
}

int   oc_plat_view_certificate(SDL_Window *owner, const uint8_t *der, size_t len) {
    (void)owner; (void)der; (void)len; return 0;
}
int   oc_plat_autostart_get(void) { return 0; }
void  oc_plat_autostart_set(int on) { (void)on; }
void  oc_plat_url_scheme_register(const char *scheme) { (void)scheme; }
void  oc_plat_sound(const char *name) { (void)name; }
uint32_t oc_plat_idle_ms(void) { return 0; }

/* ---- the clipboard --------------------------------------------------------- */

char *oc_plat_clipboard_files(void) { return NULL; }   /* a page never sees file paths */

/* An image on the clipboard, as the file the source put there (PNG or GIF).
 * Only inside a user gesture, which the paste shortcut is. */
EM_ASYNC_JS(uint8_t *, web_clipboard_image, (int *len, char *ext), {
    try {
        if (!navigator.clipboard || !navigator.clipboard.read) return 0;
        const items = await navigator.clipboard.read();
        for (const it of items) {
            if (it.types.includes("text/plain")) return 0;      /* words beat the picture of them */
            const t = it.types.find((x) => x === "image/png" || x === "image/gif");
            if (!t) continue;
            const buf = new Uint8Array(await (await it.getType(t)).arrayBuffer());
            const p = _malloc(buf.length);
            HEAPU8.set(buf, p);
            HEAP32[len >> 2] = buf.length;
            stringToUTF8(t === "image/gif" ? "gif" : "png", ext, 8);
            return p;
        }
        return 0;
    } catch (e) { return 0; }
});

uint8_t *oc_plat_clipboard_image(size_t *len, char ext[8]) {
    int n = 0; ext[0] = 0;
    uint8_t *d = web_clipboard_image(&n, ext);
    *len = d ? (size_t)n : 0;
    return d;
}

/* ---- images ---------------------------------------------------------------- */

/* The browser decodes what it can show (PNG, JPEG, GIF, WebP, BMP ...); a 2D
 * canvas reads the pixels back, premultiplied as the renderer wants them. */
EM_ASYNC_JS(uint8_t *, web_image_decode, (const uint8_t *bytes, int len, int *wp, int *hp), {
    try {
        const blob = new Blob([HEAPU8.slice(bytes, bytes + len)]);
        const bmp = await createImageBitmap(blob);
        const w = bmp.width, h = bmp.height;
        if (!w || !h || w * h > 4096 * 4096) return 0;
        const c = new OffscreenCanvas(w, h);
        const ctx = c.getContext("2d", { willReadFrequently: true });
        ctx.drawImage(bmp, 0, 0);
        bmp.close();
        const d = ctx.getImageData(0, 0, w, h).data;      /* RGBA, straight alpha */
        const p = _malloc(w * h * 4);
        const out = HEAPU8.subarray(p, p + w * h * 4);
        for (let i = 0; i < d.length; i += 4) {
            const a = d[i + 3];
            out[i]     = (d[i + 2] * a + 127) / 255;
            out[i + 1] = (d[i + 1] * a + 127) / 255;
            out[i + 2] = (d[i]     * a + 127) / 255;
            out[i + 3] = a;
        }
        HEAP32[wp >> 2] = w; HEAP32[hp >> 2] = h;
        return p;
    } catch (e) { return 0; }
});

uint8_t *oc_plat_image_decode(const uint8_t *bytes, size_t len, int *w, int *h) {
    if (!bytes || !len) return NULL;
    return web_image_decode(bytes, (int)len, w, h);
}

/* ---- text ------------------------------------------------------------------ */

st_ctx *oc_plat_text_create(const st_sink *sink) { return st_canvas_create(sink); }
