/* OpenChime GUI -- the Linux platform (client/gui/platform/platform.h).
 *
 * Everything the application needs on a Linux desktop that SDL does not give
 * it, through the freedesktop conventions rather than any one desktop's own
 * API, so it holds under GNOME, KDE and the rest, on Wayland first and on X11
 * through the same code (SDL3 carries both):
 *
 *   crash reports   a signal handler on its own stack, into the data directory
 *   crash rings     files under the data directory, mmap'd shared
 *   data directory  $XDG_DATA_HOME/openchime
 *   second launch   a Unix socket in $XDG_RUNTIME_DIR that the running
 *                   instance listens on, handed "kind\npayload"
 *   autostart       a .desktop file in $XDG_CONFIG_HOME/autostart
 *   openchime://    a .desktop file in $XDG_DATA_HOME/applications, made the
 *                   x-scheme-handler default in $XDG_CONFIG_HOME/mimeapps.list
 *   clipboard       SDL's clipboard data: text/uri-list, image/png and
 *                   image/gif, text/html
 *   file pickers    SDL3's file dialogs (the desktop portal's FileChooser)
 *   sign-in         the default browser, SDL_OpenURL
 *   tray            SDL_CreateTray (a StatusNotifierItem where the desktop
 *                   shows one)
 *   images          libjpeg-turbo, libspng and libwebp, as the daemon's
 *                   previews decode, and stb_image for GIF and BMP, behind
 *                   ARCH-137's byte and pixel caps
 *   preferences     the desktop portal's Settings interface, over GDBus
 *   idle time       GNOME's idle monitor, over GDBus
 *   text            FreeType, HarfBuzz and fontconfig (sdltext/st_ft.c)
 *
 * What Wayland forbids a client answers "not available", as the contract
 * allows: keeping a window out of captures, finding a shared window or screen
 * on the desktop, and the caption's colour, which is the compositor's. */
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <gio/gio.h>
#include <jpeglib.h>
#include <spng.h>
#include <webp/decode.h>

#include "platform.h"
#include "preview.h"        /* ARCH-137's caps, the daemon's and these the same */
#include "gfx.h"
#include "icons.h"
#include "theme.h"
#include "st_ft.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
/* stb_image is static to this file, and what of it this file does not call
 * is reported at the end of the file, whatever the state around it. */
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* The application's name on disk, in the runtime directory and to the
 * desktop: lower case, as the XDG directories' other residents are. */
#define OC_APP_DIR      "openchime"
#define OC_APP_ID       "openchime"
#define OC_SOCKET_NAME  "openchime.sock"
#define OC_AUTOSTART    "openchime.desktop"
#define OC_URL_DESKTOP  "openchime-url.desktop"
/* What a second launch may hand over: a URL or a notification's action is a
 * few hundred bytes; anything past this is not one. */
#define OC_HANDOFF_MAX  65536
/* How long a D-Bus question to the desktop may take before the answer is
 * "cannot say": these are asked on the main thread, between frames. */
#define OC_DBUS_TIMEOUT_MS 250

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

/* ---- XDG directories ---------------------------------------------------------- */

/* An XDG base directory: the variable's value `v` when it is an absolute
 * path (the specification says a relative one is to be ignored), else
 * $HOME/<fallback>. 0 when there is neither. */
static int xdg_dir(const char *v, const char *fallback, char *out, size_t cap) {
    if (v && v[0] == '/') { snprintf(out, cap, "%s", v); return 1; }
    const char *home = getenv("HOME");
    if (!home || home[0] != '/') { out[0] = 0; return 0; }
    snprintf(out, cap, "%s/%s", home, fallback);
    return 1;
}

/* mkdir -p, each directory made private to the user as the XDG
 * specification asks of the ones it names. 1 when the whole path exists. */
static int mkdirs(const char *path) {
    char p[1024];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p) return 0;
    for (char *s = p + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, 0700) != 0 && errno != EEXIST) return 0;
        *s = '/';
    }
    return mkdir(p, 0700) == 0 || errno == EEXIST;
}

/* The whole of a small file, NUL-terminated, malloc'd; NULL when it cannot be
 * read or is larger than `max`. */
static char *read_small(const char *path, size_t max, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max + 1, f) : 0;
    fclose(f);
    if (!buf || n > max) { free(buf); return NULL; }
    buf[n] = 0;
    if (len_out) *len_out = n;
    return buf;
}

/* Written beside, then renamed over: a reader never sees half a file, and a
 * crash mid-write leaves the old one. */
static int write_file(const char *path, const char *data, size_t len) {
    char tmp[1100];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return 0;
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    if (ok && rename(tmp, path) == 0) return 1;
    remove(tmp);
    return 0;
}

/* This executable's own path, which a .desktop file names. */
static int self_exe(char *out, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) { out[0] = 0; return 0; }
    out[n] = 0;
    return 1;
}

/* A path as one argument of a .desktop file's Exec key: double-quoted, with
 * the four characters the Desktop Entry specification reserves inside quotes
 * escaped, and then every backslash doubled again because the key's value is
 * itself an escaped string. */
static void exec_quote(const char *path, char *out, size_t cap) {
    size_t o = 0;
    if (cap < 3) { if (cap) out[0] = 0; return; }
    out[o++] = '"';
    for (const char *p = path; *p && o + 6 < cap; p++) {
        if (*p == '"' || *p == '`' || *p == '$') { out[o++] = '\\'; out[o++] = '\\'; }
        else if (*p == '\\') { out[o++] = '\\'; out[o++] = '\\'; out[o++] = '\\'; out[o++] = '\\'; continue; }
        out[o++] = *p;
    }
    out[o++] = '"';
    out[o] = 0;
}

/* ---- crash reports --------------------------------------------------------- */

/* A fault is written up from inside its signal handler, where nothing may be
 * allocated and the stack may be the one that overflowed. So everything the
 * handler needs exists before it runs: its own stack, the report's text
 * stream (fmemopen over a static buffer, opened at boot), the executable's
 * load address, and backtrace()'s unwinder, loaded by one call at boot. The
 * report is assembled in memory and written to its file with write(2). */
static char g_crash_dir[600];
static oc_plat_report_fn g_report_fn;
static void *g_report_user;
static char  g_crash_text[64 * 1024];
static FILE *g_crash_mem;
static uintptr_t g_exe_base;
static char  g_alt_stack[64 * 1024];
static volatile sig_atomic_t g_crashing;
static const int CRASH_SIGNALS[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };

static const char *signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    }
    return "signal";
}

static uintptr_t fault_pc(const void *ucv) {
    const ucontext_t *uc = (const ucontext_t *)ucv;
    if (!uc) return 0;
#if defined(__x86_64__)
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.pc;
#else
    return 0;
#endif
}

static void write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
        p += w; n -= (size_t)w;
    }
}

static void crash_handler(int sig, siginfo_t *si, void *uc) {
    /* A second fault while writing the first report ends the process now. */
    if (g_crashing) { signal(sig, SIG_DFL); raise(sig); return; }
    g_crashing = 1;
    char path[700];
    snprintf(path, sizeof path, "%s/crash-%ld.txt", g_crash_dir, (long)getpid());
    int fd = g_crash_dir[0] ? open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : -1;
    if (fd >= 0 && g_crash_mem) {
        FILE *f = g_crash_mem;
        rewind(f);
        uintptr_t pc = fault_pc(uc);
        fprintf(f, "openchime crash\n");
        fprintf(f, "signal=%d (%s) code=%d addr=%p pc=%p module=%p rva=0x%llx\n",
                sig, signal_name(sig), si ? si->si_code : 0, si ? si->si_addr : NULL,
                (void *)pc, (void *)g_exe_base,
                (pc && g_exe_base && pc >= g_exe_base) ? (unsigned long long)(pc - g_exe_base) : 0ull);
        if (g_report_fn) g_report_fn(f, g_report_user);
        fprintf(f, "-- stack --\n");
        fflush(f);
        long n = ftell(f);
        if (n > 0) write_all(fd, g_crash_text, (size_t)n < sizeof g_crash_text ? (size_t)n : sizeof g_crash_text);
        /* Frames as module+offset, resolved later with addr2line against the
         * build's symbols, as a minidump's are on Windows. */
        void *frames[64];
        int nf = backtrace(frames, 64);
        backtrace_symbols_fd(frames, nf, fd);
        /* The core file, where one is written, is the system's
         * (core_pattern, ulimit -c): the default action below decides. */
        close(fd);
    } else if (fd >= 0) {
        close(fd);
    }
    /* Let the process die of the signal it got: swallowing the fault would
     * leave a client running on corrupt state, which is worse than an exit
     * the user can see. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_handlers_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof CRASH_SIGNALS / sizeof CRASH_SIGNALS[0]; i++)
        sigaction(CRASH_SIGNALS[i], &sa, NULL);
}

void oc_plat_boot(void) {
    /* The main thread's overflow runs the handler on a stack of its own. */
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = g_alt_stack;
    ss.ss_size = sizeof g_alt_stack;
    sigaltstack(&ss, NULL);
    g_crash_mem = fmemopen(g_crash_text, sizeof g_crash_text, "w");
    if (g_crash_mem) setvbuf(g_crash_mem, NULL, _IONBF, 0);
    Dl_info di;
    if (dladdr((void *)oc_plat_boot, &di) && di.dli_fbase) g_exe_base = (uintptr_t)di.dli_fbase;
    { void *warm[2]; backtrace(warm, 2); }      /* loads the unwinder now, not mid-crash */
    crash_handlers_install();
    oc_plat_data_dir(g_crash_dir, sizeof g_crash_dir);
    /* The name the compositor groups the windows under and matches to the
     * .desktop files below (Wayland's app_id, X11's WM_CLASS). */
    SDL_SetHint(SDL_HINT_APP_ID, OC_APP_ID);
}

/* SDL installs handlers for the termination signals only, but nothing in its
 * contract says it never will for these: the last installer wins, so they are
 * ours again. */
void oc_plat_after_sdl_init(void) { crash_handlers_install(); }

void oc_plat_crash_reports(const char *dir, oc_plat_report_fn app_part, void *user) {
    if (dir && dir[0]) snprintf(g_crash_dir, sizeof g_crash_dir, "%s", dir);
    g_report_fn = app_part;
    g_report_user = user;
}

int oc_plat_data_dir(char *out, size_t cap) {
    char base[600];
    if (!xdg_dir(getenv("XDG_DATA_HOME"), ".local/share", base, sizeof base)) { snprintf(out, cap, "."); return 0; }
    snprintf(out, cap, "%s/" OC_APP_DIR, base);
    if (!mkdirs(out)) { snprintf(out, cap, "."); return 0; }
    return 1;
}

/* A ring is a file mapped shared: what the application writes is in the page
 * cache the moment it is written, and the kernel writes it out however the
 * process dies. The descriptor is closed at once; the mapping keeps the file. */
void *oc_plat_ring_map(const char *path, size_t size) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return NULL; }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    memset(p, 0, size);
    return p;
}

void oc_plat_ring_unmap(void *p, size_t size) {
    if (!p) return;
    msync(p, size, MS_SYNC);
    munmap(p, size);
}

/* ---- the second launch: a socket in the runtime directory ------------------ */

/* The running instance listens on $XDG_RUNTIME_DIR/openchime.sock; a second
 * launch connects, writes "kind\npayload", closes its side and waits for one
 * byte back, which says the payload was taken. The runtime directory is the
 * user's alone (0700, by the specification), and the listener checks the
 * peer's uid as well. It runs on a thread of its own, so a payload is taken
 * however busy the main loop is, and a handoff from this process itself (a
 * notification's action, arriving on a D-Bus thread) lands the same way. */
static int         g_listen_fd = -1;
static SDL_Thread *g_listen_thread;
static char        g_sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

static int sock_addr(struct sockaddr_un *a) {
    char dir[600];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || rt[0] != '/') return 0;
    snprintf(dir, sizeof dir, "%s", rt);
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    return snprintf(a->sun_path, sizeof a->sun_path, "%s/" OC_SOCKET_NAME, dir) < (int)sizeof a->sun_path;
}

static void sock_timeouts(int fd, int secs) {
    struct timeval tv = { secs, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* One connection: the whole message to EOF, checked and posted. */
static void handoff_take(int c) {
    struct ucred cr;
    socklen_t crl = sizeof cr;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &crl) != 0 || cr.uid != getuid()) return;
    sock_timeouts(c, 2);
    char *buf = malloc(OC_HANDOFF_MAX + 1);
    if (!buf) return;
    size_t n = 0;
    for (;;) {
        ssize_t r = read(c, buf + n, OC_HANDOFF_MAX + 1 - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
        if (n > OC_HANDOFF_MAX) break;
    }
    const char *nl = n <= OC_HANDOFF_MAX ? memchr(buf, '\n', n) : NULL;
    size_t kl = nl ? (size_t)(nl - buf) : 0;
    /* The two kinds a second launch carries, as on Windows. */
    if (!nl || !((kl == 3 && !memcmp(buf, "url", 3)) || (kl == 6 && !memcmp(buf, "action", 6)))) {
        free(buf);
        return;
    }
    buf[n] = 0;
    push_event(OC_PLAT_EV_HANDOFF, buf, NULL);     /* the application frees it */
    write_all(c, "1", 1);
}

static int handoff_listen(void *unused) {
    (void)unused;
    for (;;) {
        int c = accept4(g_listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return 0;                       /* shut down by oc_plat_quit */
        }
        handoff_take(c);
        close(c);
    }
}

/* Bound once, when the window exists (where Windows makes its message
 * window). A socket file left by an instance that died is found by nobody
 * answering on it, and replaced. */
static void handoff_start(void) {
    struct sockaddr_un a;
    if (g_listen_fd >= 0 || !sock_addr(&a)) return;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        if (errno != EADDRINUSE) { close(fd); return; }
        int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int alive = probe >= 0 && connect(probe, (struct sockaddr *)&a, sizeof a) == 0;
        if (probe >= 0) close(probe);
        if (alive || unlink(a.sun_path) != 0 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            close(fd);
            return;
        }
    }
    if (listen(fd, 8) != 0) { unlink(a.sun_path); close(fd); return; }
    snprintf(g_sock_path, sizeof g_sock_path, "%s", a.sun_path);
    g_listen_fd = fd;
    oc_plat_event_type();                   /* registered here, not on the thread */
    g_listen_thread = SDL_CreateThread(handoff_listen, "oc-handoff", NULL);
    if (!g_listen_thread) { unlink(g_sock_path); g_sock_path[0] = 0; close(fd); g_listen_fd = -1; }
}

static void handoff_stop(void) {
    if (g_listen_fd < 0) return;
    shutdown(g_listen_fd, SHUT_RDWR);       /* wakes accept() */
    if (g_listen_thread) SDL_WaitThread(g_listen_thread, NULL);
    g_listen_thread = NULL;
    close(g_listen_fd);
    g_listen_fd = -1;
    if (g_sock_path[0]) unlink(g_sock_path);
    g_sock_path[0] = 0;
}

int oc_plat_handoff(const char *kind, const char *payload) {
    struct sockaddr_un a;
    if (!kind || !payload || !sock_addr(&a)) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    sock_timeouts(fd, 2);
    int took = 0;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        write_all(fd, kind, strlen(kind));
        write_all(fd, "\n", 1);
        write_all(fd, payload, strlen(payload));
        shutdown(fd, SHUT_WR);
        char ack = 0;
        ssize_t r;
        do r = read(fd, &ack, 1); while (r < 0 && errno == EINTR);
        took = r == 1 && ack == '1';
    }
    close(fd);
    return took;
}

/* The desktop's back and forward are the application's own. */
int oc_plat_nav_push(uint32_t seq, int replace) { (void)seq; (void)replace; return 0; }
int oc_plat_nav_go(int delta) { (void)delta; return 0; }

/* The default browser; its result comes back to the sign-in's own listener. */
int oc_plat_open_signin(const char *url) { return SDL_OpenURL(url) ? 1 : 0; }
int oc_plat_signin_result(char *out, size_t cap) { (void)out; (void)cap; return 0; }

void oc_plat_share_limits(int *max_w, int *max_h) { *max_w = 1920; *max_h = 1080; }
void oc_plat_camera_limits(int *max_w, int *max_h, int *max_fps) { *max_w = 640; *max_h = 480; *max_fps = 30; }
/* Devices are granted through the desktop's portals, whose permissions each
 * desktop keeps in its own settings; there is no one page to open. */
const char *oc_plat_mic_settings_name(void) { return "the desktop's privacy settings"; }
const char *oc_plat_privacy_name(void) { return "The desktop"; }
int oc_plat_has_privacy_settings(void) { return 0; }
int oc_plat_open_privacy_settings(int microphone) { (void)microphone; return 0; }

/* ---- the main window ------------------------------------------------------- */

static SDL_Window *g_main;

void oc_plat_window_attach(SDL_Window *w) {
    if (!w || g_main) return;
    g_main = w;
    handoff_start();
}

void oc_plat_text_input_area(SDL_Window *w, const SDL_Rect *r) { SDL_SetTextInputArea(w, r, 0); }

/* There is no system caret to place: assistive technology reads the caret
 * from the accessibility provider's Text interface, which takes it from the
 * composer's published offsets (oc_a11y_publish), and an input method's
 * candidates follow the text input area above. */
void oc_plat_text_caret(SDL_Window *w, const SDL_Rect *r) { (void)w; (void)r; }

/* SDL_RaiseWindow asks for activation with an xdg-activation token where the
 * compositor offers the protocol (SDL_HINT_FORCE_RAISEWINDOW, which the
 * application sets, makes it ask); a compositor may still decline, which is
 * Wayland's rule against focus stealing, not an error. */
void oc_plat_window_front(SDL_Window *w) {
    if (!w) return;
    SDL_WindowFlags f = SDL_GetWindowFlags(w);
    if (f & SDL_WINDOW_HIDDEN) SDL_ShowWindow(w);
    if (f & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(w);
    SDL_RaiseWindow(w);
}

/* The decorations are the compositor's (or libdecor's), drawn in the
 * desktop's own theme: there is nothing of the caption for a client to set. */
void oc_plat_window_caption_dark(SDL_Window *w, int dark) { (void)w; (void)dark; }
/* So are the corners. */
void oc_plat_window_round(SDL_Window *w) { (void)w; }
/* Wayland gives a client no way to keep its window out of another client's
 * capture: the screen-cast portal shows what the compositor shows. */
int oc_plat_window_capture_exclude(SDL_Window *w, int on) { (void)w; (void)on; return 0; }
/* The one window that passes clicks through is the border round a shared
 * window, which is never placed here (oc_plat_share_rect answers 0). */
void oc_plat_window_click_through(SDL_Window *w, int on) { (void)w; (void)on; }
/* No taskbar badge or progress is common to the desktops: the unread count
 * stays on the application's own rail. */
int oc_plat_badge(SDL_Window *w, int count, uint32_t rgb) { (void)w; (void)count; (void)rgb; return 0; }
void oc_plat_progress(SDL_Window *w, uint64_t done, uint64_t total) { (void)w; (void)done; (void)total; }
/* A Wayland client cannot learn where another window, or a screen, is on the
 * desktop, and the portal chose the shared source without telling it; 0, so
 * no border window is placed. */
int oc_plat_share_rect(const char *id, int *x, int *y, int *w, int *h) {
    (void)id; (void)x; (void)y; (void)w; (void)h;
    return 0;
}

/* ---- the tray ------------------------------------------------------------- */

/* SDL's tray is an icon with a menu: the desktop's status area reports no
 * clicks on the icon itself. The menu's two entries are the two clicks the
 * application knows: open the window, and open it with the application's own
 * menu (ARCH-98 keeps menus inside the window). */
static SDL_Tray *g_tray;

static void tray_open_cb(void *ud, SDL_TrayEntry *e) { (void)ud; (void)e; push_event(OC_PLAT_EV_TRAY, (void *)(intptr_t)OC_TRAY_CLICK, NULL); }
static void tray_more_cb(void *ud, SDL_TrayEntry *e) { (void)ud; (void)e; push_event(OC_PLAT_EV_TRAY, (void *)(intptr_t)OC_TRAY_RCLICK, NULL); }

/* The application's mark, drawn as scripts/gen/appicon.py draws the Windows
 * icon: the accent rounded square with Lucide's bell in white, here through
 * the same drawing layer the window uses, into a surface of its own. */
static SDL_Surface *tray_icon(void) {
    const int s = 64;
    SDL_Surface *sf = SDL_CreateSurface(s, s, SDL_PIXELFORMAT_ARGB8888);
    if (!sf) return NULL;
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(sf);
    gfx *g = r ? gfx_create(r) : NULL;
    if (!g) { if (r) SDL_DestroyRenderer(r); SDL_DestroySurface(sf); return NULL; }
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    gfx_fill_round(g, (gfx_rect){ 2, 2, (float)s - 4, (float)s - 4 }, (float)s * 0.22f, OC_COL_ACCENT, 1.0f);
    gfx_icon(g, OC_ICON_BELL, (gfx_rect){ (float)s * 0.2f, (float)s * 0.2f, (float)s * 0.6f, (float)s * 0.6f },
             (float)s / 16.0f, 0xFFFFFF, 1.0f);
    SDL_FlushRenderer(r);
    gfx_destroy(g);
    SDL_DestroyRenderer(r);
    return sf;
}

int oc_plat_tray_init(SDL_Window *w, const char *tooltip) {
    (void)w;
    if (g_tray) return 1;
    SDL_Surface *icon = tray_icon();
    g_tray = SDL_CreateTray(icon, tooltip ? tooltip : "OpenChime");
    if (icon) SDL_DestroySurface(icon);
    if (!g_tray) return 0;
    SDL_TrayMenu *m = SDL_CreateTrayMenu(g_tray);
    SDL_TrayEntry *open = m ? SDL_InsertTrayEntryAt(m, -1, "Open OpenChime", SDL_TRAYENTRY_BUTTON) : NULL;
    SDL_TrayEntry *more = m ? SDL_InsertTrayEntryAt(m, -1, "More\xE2\x80\xA6", SDL_TRAYENTRY_BUTTON) : NULL;
    if (!open || !more) { SDL_DestroyTray(g_tray); g_tray = NULL; return 0; }
    SDL_SetTrayEntryCallback(open, tray_open_cb, NULL);
    SDL_SetTrayEntryCallback(more, tray_more_cb, NULL);
    return 1;
}

void oc_plat_tray_done(void) {
    if (g_tray) { SDL_DestroyTray(g_tray); g_tray = NULL; }
}

/* Notifications go to the desktop's notification server, not through the
 * tray: there is no balloon for them to ride on. */
void *oc_plat_tray_notify_handle(unsigned *tray_id) {
    if (tray_id) *tray_id = 0;
    return NULL;
}

/* ---- files: SDL3's dialogs ------------------------------------------------- */

/* SDL's dialogs answer through a callback -- the portal's from the event pump
 * on this thread, its zenity fallback's from a thread of its own -- and the
 * contract's pickers return the answer, so the call waits, pumping SDL's
 * events (they stay queued for the application) until the callback has run. */
typedef struct {
    SDL_AtomicInt done;
    char  *out;
    size_t cap;
    int    n;
} pick_wait;

static void pick_cb(void *ud, const char *const *list, int filter) {
    (void)filter;
    pick_wait *pw = (pick_wait *)ud;
    size_t o = 0;
    if (list)
        for (int i = 0; list[i]; i++) {
            size_t l = strlen(list[i]);
            if (o + l + 2 > pw->cap) break;
            memcpy(pw->out + o, list[i], l + 1);
            o += l + 1;
            pw->n++;
        }
    if (pw->cap) pw->out[o] = 0;
    SDL_SetAtomicInt(&pw->done, 1);
}

static void pick_wait_for(pick_wait *pw) {
    while (!SDL_GetAtomicInt(&pw->done)) {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
}

int oc_plat_pick_files(SDL_Window *owner, const char *filter, int multi, char *out, size_t cap) {
    if (!out || cap < 2) return 0;
    out[0] = out[1] = 0;
    /* "Label|*.png;*.jpg" -> the label, and "png;jpg" as SDL's pattern. */
    char label[128] = "", pat[256] = "";
    SDL_DialogFileFilter f = { label, pat };
    int nf = 0;
    const char *bar = filter ? strchr(filter, '|') : NULL;
    if (bar) {
        snprintf(label, sizeof label, "%.*s", (int)(bar - filter), filter);
        size_t o = 0;
        for (const char *p = bar + 1; *p && o + 1 < sizeof pat; p++) {
            if (*p == '*' || (*p == '.' && (p == bar + 1 || p[-1] == '*'))) continue;
            pat[o++] = *p;
        }
        pat[o] = 0;
        nf = pat[0] ? 1 : 0;
    }
    pick_wait pw;
    memset(&pw, 0, sizeof pw);
    pw.out = out; pw.cap = cap;
    SDL_ShowOpenFileDialog(pick_cb, &pw, owner, nf ? &f : NULL, nf, NULL, multi ? true : false);
    pick_wait_for(&pw);
    if (!pw.n) out[0] = out[1] = 0;
    return pw.n;
}

int oc_plat_pick_save(SDL_Window *owner, const char *suggested, char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    /* Offered in Downloads under the suggested name: a folder that exists and
     * a file that does not, which the portal opens as a new file named so. */
    char where[1200] = "";
    const char *dl = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
    const char *home = getenv("HOME");
    const char *dir = dl ? dl : home;
    if (dir && suggested && suggested[0])
        snprintf(where, sizeof where, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", suggested);
    else if (dir)
        snprintf(where, sizeof where, "%s", dir);
    char buf[2048];
    pick_wait pw;
    memset(&pw, 0, sizeof pw);
    pw.out = buf; pw.cap = sizeof buf;
    SDL_ShowSaveFileDialog(pick_cb, &pw, owner, NULL, 0, where[0] ? where : NULL);
    pick_wait_for(&pw);
    if (!pw.n) return 0;
    snprintf(out, cap, "%s", buf);
    return 1;
}

void oc_plat_file_saved(const char *path) { (void)path; }   /* the picker already chose where */

/* No certificate viewer is common to the desktops. */
int oc_plat_view_certificate(SDL_Window *owner, const uint8_t *der, size_t len) {
    (void)owner; (void)der; (void)len;
    return 0;
}

/* ---- autostart and the URL scheme: .desktop files ------------------------- */

static int autostart_path(char *out, size_t cap) {
    char cfg[600];
    if (!xdg_dir(getenv("XDG_CONFIG_HOME"), ".config", cfg, sizeof cfg)) return 0;
    return snprintf(out, cap, "%s/autostart/" OC_AUTOSTART, cfg) < (int)cap;
}

/* On when the file is there and not switched off inside (Hidden=true is how
 * a desktop's own settings turn an entry off without deleting it). */
int oc_plat_autostart_get(void) {
    char p[700];
    if (!autostart_path(p, sizeof p)) return 0;
    char *d = read_small(p, 64 * 1024, NULL);
    if (!d) return 0;
    int on = !strstr(d, "\nHidden=true") && !strstr(d, "\nX-GNOME-Autostart-enabled=false");
    free(d);
    return on;
}

void oc_plat_autostart_set(int on) {
    char p[700], exe[600], q[1300];
    if (!autostart_path(p, sizeof p)) return;
    if (!on) { remove(p); return; }
    if (!self_exe(exe, sizeof exe)) return;
    char dir[700];
    snprintf(dir, sizeof dir, "%s", p);
    *strrchr(dir, '/') = 0;
    if (!mkdirs(dir)) return;
    exec_quote(exe, q, sizeof q);
    char body[1600];
    int n = snprintf(body, sizeof body,
                     "[Desktop Entry]\n"
                     "Type=Application\n"
                     "Name=OpenChime\n"
                     "Exec=%s\n"
                     "Terminal=false\n"
                     "X-GNOME-Autostart-enabled=true\n", q);
    if (n > 0 && n < (int)sizeof body) write_file(p, body, (size_t)n);
}

/* `key=value` in the [Default Applications] group of a mimeapps.list,
 * replacing the key where it is and adding it (and the group) where it is
 * not; every other line kept as it was. NULL on failure. */
static char *mimeapps_with(const char *old, const char *key, const char *value) {
    size_t ol = old ? strlen(old) : 0, kl = strlen(key);
    size_t cap = ol + kl + strlen(value) + 64;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    int in_group = 0, done = 0;
    const char *p = old ? old : "";
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        int header = ll && p[0] == '[';
        if (header && in_group && !done) {
            o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
            done = 1;
        }
        if (header) in_group = ll == 22 && !memcmp(p, "[Default Applications]", 22);
        if (in_group && !done && ll > kl && !memcmp(p, key, kl) && p[kl] == '=') {
            o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
            done = 1;
        } else {
            memcpy(out + o, p, ll);
            o += ll;
            out[o++] = '\n';
        }
        p += ll + (eol ? 1 : 0);
    }
    if (!done) {
        if (in_group) o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, value);
        else o += (size_t)snprintf(out + o, cap - o, "%s[Default Applications]\n%s=%s\n", o ? "\n" : "", key, value);
    }
    out[o] = 0;
    return out;
}

/* Under the user's own directories, as Windows' is under HKCU: no root, and
 * it follows the person. The entry names this executable, written on every
 * start, so a developer build or a moved copy is the one a link opens. It is
 * kept out of the application menus (NoDisplay): it exists for links, and the
 * menu's entry is a package's to install. */
void oc_plat_url_scheme_register(const char *scheme) {
    char data[600], cfg[600], exe[600], q[1300], dir[700], path[800], list[800];
    if (!scheme || !scheme[0] || !self_exe(exe, sizeof exe)) return;
    if (!xdg_dir(getenv("XDG_DATA_HOME"), ".local/share", data, sizeof data) ||
        !xdg_dir(getenv("XDG_CONFIG_HOME"), ".config", cfg, sizeof cfg)) return;
    snprintf(dir, sizeof dir, "%s/applications", data);
    snprintf(path, sizeof path, "%s/" OC_URL_DESKTOP, dir);
    if (!mkdirs(dir) || !mkdirs(cfg)) return;
    exec_quote(exe, q, sizeof q);
    char body[1800];
    int n = snprintf(body, sizeof body,
                     "[Desktop Entry]\n"
                     "Type=Application\n"
                     "Name=OpenChime\n"
                     "Exec=%s %%u\n"
                     "Terminal=false\n"
                     "NoDisplay=true\n"
                     "MimeType=x-scheme-handler/%s;\n", q, scheme);
    if (n <= 0 || n >= (int)sizeof body) return;
    size_t ol = 0;
    char *old = read_small(path, 64 * 1024, &ol);
    int same = old && ol == (size_t)n && !memcmp(old, body, ol);
    free(old);
    if (!same && !write_file(path, body, (size_t)n)) return;
    char key[128];
    snprintf(key, sizeof key, "x-scheme-handler/%s", scheme);
    snprintf(list, sizeof list, "%s/mimeapps.list", cfg);
    char *cur = read_small(list, 1024 * 1024, NULL);
    char *next = mimeapps_with(cur, key, OC_URL_DESKTOP);
    if (next && (!cur || strcmp(cur, next))) write_file(list, next, strlen(next));
    free(cur);
    free(next);
}

/* The notification server plays the sound named with each notification (the
 * freedesktop sound theme's names, osnotify.h); a sound the client plays by
 * name itself would need a sound-theme player this build does not link. */
void oc_plat_sound(const char *name) { (void)name; }

/* ---- the desktop, over D-Bus ------------------------------------------------ */

/* The session bus, once: a desktop without one answers "cannot say" to every
 * question below, and is asked only once. */
static GDBusConnection *session_bus(void) {
    static GDBusConnection *bus;
    static int tried;
    if (!tried) {
        tried = 1;
        bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    }
    return bus;
}

/* One value of the desktop portal's Settings (org.freedesktop.portal.Settings):
 * ReadOne where the portal has it (version 2), else Read, whose answer is the
 * value wrapped once more. NULL when the portal, or the setting, is absent. */
static GVariant *portal_setting(const char *ns, const char *key) {
    GDBusConnection *bus = session_bus();
    if (!bus) return NULL;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                              "org.freedesktop.portal.Settings", "ReadOne",
                                              g_variant_new("(ss)", ns, key), G_VARIANT_TYPE("(v)"),
                                              G_DBUS_CALL_FLAGS_NONE, OC_DBUS_TIMEOUT_MS, NULL, &err);
    int wrapped = 0;
    if (!r && err && g_error_matches(err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
        g_clear_error(&err);
        r = g_dbus_connection_call_sync(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                        "org.freedesktop.portal.Settings", "Read",
                                        g_variant_new("(ss)", ns, key), G_VARIANT_TYPE("(v)"),
                                        G_DBUS_CALL_FLAGS_NONE, OC_DBUS_TIMEOUT_MS, NULL, &err);
        wrapped = 1;
    }
    g_clear_error(&err);
    if (!r) return NULL;
    GVariant *v = NULL;
    g_variant_get(r, "(v)", &v);
    g_variant_unref(r);
    if (wrapped && v && g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)) {
        GVariant *in = g_variant_get_variant(v);
        g_variant_unref(v);
        v = in;
    }
    return v;
}

/* GNOME's idle monitor (org.gnome.Mutter.IdleMonitor), the one idle question
 * a Wayland client can ask a compositor and be answered in time; elsewhere
 * there is no protocol for it, and the answer is 0, "unknown". */
uint32_t oc_plat_idle_ms(void) {
    GDBusConnection *bus = session_bus();
    if (!bus) return 0;
    GVariant *r = g_dbus_connection_call_sync(bus, "org.gnome.Mutter.IdleMonitor", "/org/gnome/Mutter/IdleMonitor/Core",
                                              "org.gnome.Mutter.IdleMonitor", "GetIdletime", NULL,
                                              G_VARIANT_TYPE("(t)"), G_DBUS_CALL_FLAGS_NONE, OC_DBUS_TIMEOUT_MS,
                                              NULL, NULL);
    if (!r) return 0;
    guint64 ms = 0;
    g_variant_get(r, "(t)", &ms);
    g_variant_unref(r);
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

/* GNOME's "Animations" switch (org.gnome.desktop.interface enable-animations,
 * which the portal carries for every desktop that sets it): off means reduce. */
int oc_plat_reduce_motion(void) {
    GVariant *v = portal_setting("org.gnome.desktop.interface", "enable-animations");
    if (!v) return -1;
    int r = g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) ? (g_variant_get_boolean(v) ? 0 : 1) : -1;
    g_variant_unref(v);
    return r;
}

/* The appearance namespace's contrast: 0 no preference, 1 higher contrast. */
int oc_plat_high_contrast(void) {
    GVariant *v = portal_setting("org.freedesktop.appearance", "contrast");
    if (!v) return -1;
    int r = g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32) ? (g_variant_get_uint32(v) == 1 ? 1 : 0) : -1;
    g_variant_unref(v);
    return r;
}

/* The appearance namespace's color-scheme: 0 no preference, 1 prefer dark,
 * 2 prefer light. No preference is "cannot say", as an absent setting is. */
int oc_plat_prefers_light(void) {
    GVariant *v = portal_setting("org.freedesktop.appearance", "color-scheme");
    if (!v) return -1;
    int r = -1;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) {
        guint32 s = g_variant_get_uint32(v);
        r = s == 2 ? 1 : s == 1 ? 0 : -1;
    }
    g_variant_unref(v);
    return r;
}

/* ---- the clipboard: SDL's clipboard data ----------------------------------- */

/* SDL's clipboard is the desktop's (Wayland's data device, X11's
 * selections), text and every other type alike. */
void oc_plat_clipboard_set_text(const char *utf8) { SDL_SetClipboardText(utf8 ? utf8 : ""); }

/* The clipboard's bytes of one type, NUL-terminated past `len` so text can be
 * read as a string; NULL when the type is not there or is over 64 MiB. */
static uint8_t *clip_bytes(const char *mime, size_t *len) {
    *len = 0;
    if (!SDL_HasClipboardData(mime)) return NULL;
    size_t n = 0;
    void *d = SDL_GetClipboardData(mime, &n);
    if (!d || !n || n > 64u * 1024u * 1024u) { SDL_free(d); return NULL; }
    uint8_t *out = malloc(n + 1);
    if (out) { memcpy(out, d, n); out[n] = 0; *len = n; }
    SDL_free(d);
    return out;
}

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* text/uri-list (RFC 2483): one URI a line, CRLF, '#' for a comment. Only
 * local files are files: file:///path or file://localhost/path, percent-
 * decoded. */
char *oc_plat_clipboard_files(void) {
    size_t n = 0;
    uint8_t *d = clip_bytes("text/uri-list", &n);
    if (!d) return NULL;
    char *out = malloc(n + 2);
    size_t o = 0;
    if (out) {
        char *p = (char *)d;
        while (*p) {
            char *eol = p + strcspn(p, "\r\n");
            char save = *eol;
            *eol = 0;
            const char *path = NULL;
            if (!strncmp(p, "file:///", 8)) path = p + 7;
            else if (!strncmp(p, "file://localhost/", 17)) path = p + 16;
            if (p[0] != '#' && path) {
                size_t start = o;
                for (const char *s = path; *s; s++) {
                    int h1, h2;
                    if (*s == '%' && (h1 = hexval(s[1])) >= 0 && (h2 = hexval(s[2])) >= 0) {
                        out[o++] = (char)(h1 * 16 + h2);
                        s += 2;
                    } else out[o++] = *s;
                }
                out[o] = 0;
                /* A decoded NUL would cut a path short: such a line is no file. */
                if (strlen(out + start) != o - start) o = start;
                else o++;
            }
            *eol = save;
            p = eol;
            while (*p == '\r' || *p == '\n') p++;
        }
        out[o] = 0;
        if (!o) { free(out); out = NULL; }
    }
    free(d);
    return out;
}

/* As the source put it there: a GIF's own bytes, else a PNG -- what every
 * toolkit offers an image as. Without `with_text`, anything that also carries
 * text is text, not an image. */
static uint8_t *clip_image(size_t *len, char ext[8], int with_text) {
    *len = 0; ext[0] = 0;
    if (!with_text && SDL_HasClipboardText()) return NULL;
    uint8_t *d = clip_bytes("image/gif", len);
    if (d) { snprintf(ext, 8, "gif"); return d; }
    d = clip_bytes("image/png", len);
    if (d) { snprintf(ext, 8, "png"); return d; }
    return NULL;
}

uint8_t *oc_plat_clipboard_image(size_t *len, char ext[8])   { return clip_image(len, ext, 0); }
uint8_t *oc_plat_clipboard_picture(size_t *len, char ext[8]) { return clip_image(len, ext, 1); }

/* text/html, as UTF-8. Firefox has offered it as UTF-16 with a byte order
 * mark; that is converted, and a UTF-8 mark dropped. */
char *oc_plat_clipboard_html(void) {
    size_t n = 0;
    uint8_t *d = clip_bytes("text/html", &n);
    if (!d) return NULL;
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
        char *u = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)d + 2, n - 2);
        free(d);
        if (!u) return NULL;
        char *out = strdup(u);
        SDL_free(u);
        return out;
    }
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) memmove(d, d + 3, n - 2);
    return (char *)d;
}

/* ---- images ---------------------------------------------------------------- */

/* Every decode is held to ARCH-137's caps, the daemon's own for the same
 * untrusted bytes: no more than OC_PREVIEW_MAX_BYTES_DEFAULT of input, and the
 * dimensions read from the header and refused over
 * OC_PREVIEW_MAX_PIXELS_DEFAULT before any pixel buffer is allocated. JPEG,
 * PNG and WebP go through the hardened decoders the daemon uses
 * (daemon/preview.c); GIF and BMP, which the daemon never decodes, through
 * stb_image, whose first frame of a GIF is the still a picker shows. The
 * result is premultiplied BGRA, stride w*4. */
static int over_cap(uint64_t w, uint64_t h) {
    return !w || !h || w * h > OC_PREVIEW_MAX_PIXELS_DEFAULT;
}

/* Straight RGBA (or BGRA with `bgr`) to premultiplied BGRA, in place. */
static void premultiply(uint8_t *px, size_t n, int bgr) {
    for (size_t i = 0; i < n; i++, px += 4) {
        unsigned a = px[3];
        uint8_t c0 = px[0], c2 = px[2];
        if (!bgr) { px[0] = c2; px[2] = c0; }
        if (a != 255) {
            px[0] = (uint8_t)((px[0] * a + 127) / 255);
            px[1] = (uint8_t)((px[1] * a + 127) / 255);
            px[2] = (uint8_t)((px[2] * a + 127) / 255);
        }
    }
}

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jmp;
} jpeg_err;

static void jpeg_fail(j_common_ptr c) { longjmp(((jpeg_err *)c->err)->jmp, 1); }
static void jpeg_quiet(j_common_ptr c) { (void)c; }

/* libjpeg-turbo writes BGRA itself (JCS_EXT_BGRA), opaque. CMYK and YCCK are
 * print files and are refused, as the daemon refuses them. */
static uint8_t *decode_jpeg(const uint8_t *b, size_t n, int *w, int *h) {
    struct jpeg_decompress_struct ci;
    jpeg_err je;
    uint8_t *volatile px = NULL;
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jpeg_fail;
    je.pub.output_message = jpeg_quiet;
    if (setjmp(je.jmp)) { jpeg_destroy_decompress(&ci); free(px); return NULL; }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, b, (unsigned long)n);
    jpeg_read_header(&ci, TRUE);
    if (over_cap(ci.image_width, ci.image_height) ||
        (ci.jpeg_color_space != JCS_YCbCr && ci.jpeg_color_space != JCS_RGB && ci.jpeg_color_space != JCS_GRAYSCALE)) {
        jpeg_destroy_decompress(&ci);
        return NULL;
    }
    ci.out_color_space = JCS_EXT_BGRA;
    jpeg_start_decompress(&ci);
    size_t stride = (size_t)ci.output_width * 4;
    px = malloc(stride * ci.output_height);
    if (!px) longjmp(je.jmp, 1);
    while (ci.output_scanline < ci.output_height) {
        JSAMPROW row = px + stride * ci.output_scanline;
        if (jpeg_read_scanlines(&ci, &row, 1) != 1) longjmp(je.jmp, 1);
    }
    *w = (int)ci.output_width;
    *h = (int)ci.output_height;
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    return px;
}

/* The IHDR's dimensions first; libspng's own chunk limits as the daemon sets
 * them. */
static uint8_t *decode_png(const uint8_t *b, size_t n, int *w, int *h) {
    spng_ctx *ctx = spng_ctx_new(0);
    if (!ctx) return NULL;
    uint8_t *px = NULL;
    struct spng_ihdr ihdr;
    size_t size = 0;
    spng_set_chunk_limits(ctx, 8u << 20, 16u << 20);
    if (spng_set_png_buffer(ctx, b, n) || spng_get_ihdr(ctx, &ihdr) || over_cap(ihdr.width, ihdr.height) ||
        spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &size) || size != (size_t)ihdr.width * ihdr.height * 4)
        goto done;
    px = malloc(size);
    if (px && spng_decode_image(ctx, px, size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS)) { free(px); px = NULL; }
    if (px) {
        premultiply(px, (size_t)ihdr.width * ihdr.height, 0);
        *w = (int)ihdr.width;
        *h = (int)ihdr.height;
    }
done:
    spng_ctx_free(ctx);
    return px;
}

/* libwebp writes premultiplied BGRA itself (MODE_bgrA). An animation's first
 * frame is not what it shows, and the plain decoder refuses one. */
static uint8_t *decode_webp(const uint8_t *b, size_t n, int *w, int *h) {
    WebPDecoderConfig cfg;
    if (!WebPInitDecoderConfig(&cfg) || WebPGetFeatures(b, n, &cfg.input) != VP8_STATUS_OK ||
        cfg.input.has_animation || over_cap((uint64_t)cfg.input.width, (uint64_t)cfg.input.height))
        return NULL;
    size_t stride = (size_t)cfg.input.width * 4, size = stride * (size_t)cfg.input.height;
    uint8_t *px = malloc(size);
    if (!px) return NULL;
    cfg.output.colorspace = MODE_bgrA;
    cfg.output.is_external_memory = 1;
    cfg.output.u.RGBA.rgba = px;
    cfg.output.u.RGBA.stride = (int)stride;
    cfg.output.u.RGBA.size = size;
    if (WebPDecode(b, n, &cfg) != VP8_STATUS_OK) { WebPFreeDecBuffer(&cfg.output); free(px); return NULL; }
    WebPFreeDecBuffer(&cfg.output);
    *w = cfg.input.width;
    *h = cfg.input.height;
    return px;
}

static uint8_t *decode_stb(const uint8_t *b, size_t n, int *w, int *h) {
    int iw = 0, ih = 0, ch = 0;
    if (!stbi_info_from_memory(b, (int)n, &iw, &ih, &ch) || iw <= 0 || ih <= 0 || over_cap((uint64_t)iw, (uint64_t)ih))
        return NULL;
    uint8_t *rgba = stbi_load_from_memory(b, (int)n, &iw, &ih, &ch, 4);
    if (!rgba) return NULL;
    size_t size = (size_t)iw * (size_t)ih * 4;
    uint8_t *px = malloc(size);
    if (px) {
        memcpy(px, rgba, size);
        premultiply(px, (size_t)iw * (size_t)ih, 0);
        *w = iw;
        *h = ih;
    }
    stbi_image_free(rgba);
    return px;
}

uint8_t *oc_plat_image_decode(const uint8_t *b, size_t n, int *w, int *h) {
    if (!b || !n || n > OC_PREVIEW_MAX_BYTES_DEFAULT) return NULL;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return decode_jpeg(b, n, w, h);
    if (n >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) return decode_png(b, n, w, h);
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4)) return decode_webp(b, n, w, h);
    if (n >= 6 && (!memcmp(b, "GIF87a", 6) || !memcmp(b, "GIF89a", 6))) return decode_stb(b, n, w, h);
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') return decode_stb(b, n, w, h);
    return NULL;
}

/* ---- text ------------------------------------------------------------------ */

st_ctx *oc_plat_text_create(const st_sink *sink) { return st_ft_create(sink); }

void oc_plat_quit(void) {
    oc_plat_tray_done();
    handoff_stop();
}

const char *oc_plat_name(void) { return "linux"; }
void oc_plat_frame_shown(void) {}   /* nothing loads ahead of the window here */
