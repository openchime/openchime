/* The UI harness's driver (driver.h). */
#include "driver.h"

#include "config.h"
#include "dbwriter.h"
#include "netloop.h"
#include "protocol.h"
#include "roles.h"
#include "testsignin.h"
#include "tls.h"
#include "spng.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static void mkdirs(const char *path);

/* The address the client and the driver meet on: OC_UI_HOST when set; under
 * WSL, this machine's own address on its virtual network, which Windows
 * reaches directly (127.0.0.1 would go through WSL's forwarding of localhost,
 * which takes up a newly opened port only some time later); else 127.0.0.1. */
static const char *ui_host(void) {
    static char host[64];
    if (host[0]) return host;
    const char *e = getenv("OC_UI_HOST");
    snprintf(host, sizeof host, "%s", e && *e ? e : "127.0.0.1");
    if (e && *e) return host;
    if (access("/proc/sys/fs/binfmt_misc/WSLInterop", F_OK) != 0) return host;
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return host;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK) &&
            (i->ifa_flags & IFF_UP)) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, host, sizeof host);
            break;
        }
    freeifaddrs(ifs);
    return host;
}

const ui_settings UI_DEFAULT = { 96, 1120, 820, "UTC", "dark", 1, "on", 1 };

/* ---- the daemon ------------------------------------------------------------ */

struct ui_daemon {
    oc_tls_server         srv;
    oc_dbwriter          *dbw;
    pthread_t             th;
    volatile sig_atomic_t stop;
    oc_netloop_view       view;
    int                   exited, port;
    uint8_t               fp[OC_TLS_FINGERPRINT_LEN];
    char                  mode_was[32], data_was[512];
    int                   had_data;
};

static void *ui_loop(void *p) {
    ui_daemon *d = p;
    oc_netloop_run_ex(0, &d->srv, d->dbw, &d->stop, &d->view);
    __atomic_store_n(&d->exited, 1, __ATOMIC_RELEASE);
    return NULL;
}

ui_daemon *ui_daemon_start(const char *db_name, uint64_t wall_ms) {
    ui_daemon *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    const char *was = getenv("OPENCHIME_MODE");
    snprintf(d->mode_was, sizeof d->mode_was, "%s", was ? was : "");
    setenv("OPENCHIME_MODE", "development", 1);
    setenv("OPENCHIME_DEV_TEST_HOOKS", "on", 1);          /* the SYNC barrier */
    /* Its own data directory, empty: blobs, identity, certificate. */
    const char *dw = getenv("OPENCHIME_DATA_DIR");
    d->had_data = dw != NULL;
    snprintf(d->data_was, sizeof d->data_was, "%s", dw ? dw : "");
    char cwd[256], data[512];
    if (!getcwd(cwd, sizeof cwd)) { free(d); return NULL; }
    snprintf(data, sizeof data, "%s/build/ui-out/%s.data/", cwd, db_name);
    char rm[600];
    snprintf(rm, sizeof rm, "rm -rf '%s'", data);
    if (system(rm) != 0) { free(d); return NULL; }
    mkdirs(data);
    setenv("OPENCHIME_DATA_DIR", data, 1);
    /* Named, so the screen does not show the address the run meets on. */
    setenv("OPENCHIME_WORKSPACE_NAME", "Harness", 1);
    char err[160];
    if (oc_config_load(err, sizeof err) != 0) { fprintf(stderr, "ui: config: %s\n", err); free(d); return NULL; }
    if (oc_tls_server_init(&d->srv, NULL, NULL) != 0) { free(d); return NULL; }
    char path[512];
    snprintf(path, sizeof path, "build/ui-out/%s", db_name);
    mkdir("build/ui-out", 0755);
    unlink(path);
    char side[560];
    snprintf(side, sizeof side, "%s-wal", path); unlink(side);
    snprintf(side, sizeof side, "%s-shm", path); unlink(side);
    oc_dbwriter_hold_clock_ms(wall_ms);
    d->dbw = oc_dbwriter_start(path);
    if (!d->dbw) { oc_tls_server_free(&d->srv); free(d); return NULL; }
    oc_dbwriter_set_pw_mem(d->dbw, 2048);
    if (!oc_dbwriter_register_local(d->dbw, "alice", "pw-alice", OC_ROLE_OWNER, 2048) ||
        !oc_dbwriter_register_local(d->dbw, "bob", "pw-bob", OC_ROLE_MEMBER, 2048) ||
        !oc_dbwriter_register_local(d->dbw, "carol", "pw-carol", OC_ROLE_MEMBER, 2048)) {
        ui_daemon_stop(d);
        return NULL;
    }
    if (pthread_create(&d->th, NULL, ui_loop, d) != 0) { oc_dbwriter_stop(d->dbw); oc_tls_server_free(&d->srv); free(d); return NULL; }
    /* Until the loop says which port it bound, or that it stopped: its own
     * account, not a time. */
    while (!__atomic_load_n(&d->view.proto_port, __ATOMIC_ACQUIRE) && !__atomic_load_n(&d->exited, __ATOMIC_ACQUIRE))
        sched_yield();
    d->port = __atomic_load_n(&d->view.proto_port, __ATOMIC_ACQUIRE);
    if (!d->port || oc_tls_server_fingerprint(&d->srv, d->fp) != 0) { ui_daemon_stop(d); return NULL; }
    return d;
}

int ui_daemon_port(const ui_daemon *d) { return d ? d->port : 0; }

void ui_daemon_advance(ui_daemon *d, uint64_t ms) { (void)d; oc_dbwriter_advance_clock_ms(ms); }
void ui_daemon_sweep(ui_daemon *d) { if (d && d->dbw) oc_dbwriter_sweep_now(d->dbw); }

void ui_daemon_stop(ui_daemon *d) {
    if (!d) return;
    if (d->port) {
        __atomic_store_n(&d->stop, 1, __ATOMIC_RELEASE);
        pthread_join(d->th, NULL);
    }
    if (d->dbw) oc_dbwriter_stop(d->dbw);
    oc_tls_server_free(&d->srv);
    oc_dbwriter_advance_clock_ms(0);
    oc_dbwriter_hold_clock_ms(0);
    unsetenv("OPENCHIME_DEV_TEST_HOOKS");
    unsetenv("OPENCHIME_WORKSPACE_NAME");
    if (d->had_data) setenv("OPENCHIME_DATA_DIR", d->data_was, 1); else unsetenv("OPENCHIME_DATA_DIR");
    if (d->mode_was[0]) setenv("OPENCHIME_MODE", d->mode_was, 1); else unsetenv("OPENCHIME_MODE");
    char err[160];
    oc_config_load(err, sizeof err);
    free(d);
}

/* ---- the client ------------------------------------------------------------- */

struct ui_client {
    pid_t    pid;
    int      pidfd, lfd, fd;
    char     rbuf[1 << 16];
    size_t   rlen;
    uint64_t frame;
    int      gone;
};

static int send_all(int fd, const char *s, size_t n) {
    while (n) {
        ssize_t w = send(fd, s, n, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        s += w; n -= (size_t)w;
    }
    return 0;
}

/* The next line from the client, or -1 when it has gone. */
static int read_line(ui_client *c, char *out, size_t cap) {
    for (;;) {
        char *nl = memchr(c->rbuf, '\n', c->rlen);
        if (nl) {
            size_t n = (size_t)(nl - c->rbuf), k = n < cap - 1 ? n : cap - 1;
            memcpy(out, c->rbuf, k); out[k] = '\0';
            memmove(c->rbuf, nl + 1, c->rlen - n - 1);
            c->rlen -= n + 1;
            return (int)k;
        }
        if (c->rlen == sizeof c->rbuf) return -1;
        ssize_t r = recv(c->fd, c->rbuf + c->rlen, sizeof c->rbuf - c->rlen, 0);
        if (r <= 0) return -1;
        c->rlen += (size_t)r;
    }
}

static int read_bytes(ui_client *c, uint8_t *out, size_t n) {
    size_t have = c->rlen < n ? c->rlen : n;
    memcpy(out, c->rbuf, have);
    memmove(c->rbuf, c->rbuf + have, c->rlen - have);
    c->rlen -= have;
    while (have < n) {
        ssize_t r = recv(c->fd, out + have, n - have, 0);
        if (r <= 0) return -1;
        have += (size_t)r;
    }
    return 0;
}

static void client_reap(ui_client *c) {
    if (c->pid > 0) { int st; waitpid(c->pid, &st, 0); c->pid = 0; }
}

ui_client *ui_client_start_tokens(const ui_settings *s, uint64_t wall_ms, const char *present, const char *expect) {
    ui_client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fd = c->lfd = c->pidfd = -1;
    c->lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    if (inet_pton(AF_INET, ui_host(), &a.sin_addr) != 1) { ui_client_stop(c); return NULL; }
    socklen_t al = sizeof a;
    if (c->lfd < 0 || bind(c->lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(c->lfd, 1) != 0 ||
        getsockname(c->lfd, (struct sockaddr *)&a, &al) != 0) { ui_client_stop(c); return NULL; }
    char port[16];
    snprintf(port, sizeof port, "%d", ntohs(a.sin_port));
    const char *vis = getenv("OC_UI_VISIBLE");
    int visible = vis && !strcmp(vis, "1");
    char *argv[10] = { "build/openchime.exe", "--harness", port, "--harness-token", (char *)present,
                       "--harness-host", (char *)ui_host(), NULL, NULL, NULL };
    if (!visible) argv[7] = "--harness-headless";
    if (posix_spawn(&c->pid, argv[0], NULL, NULL, argv, environ) != 0) { c->pid = 0; ui_client_stop(c); return NULL; }
    c->pidfd = (int)syscall(SYS_pidfd_open, c->pid, 0);
    /* The client's connection, or its end: whichever comes. */
    struct pollfd pf[2] = { { c->lfd, POLLIN, 0 }, { c->pidfd, POLLIN, 0 } };
    for (;;) {
        if (poll(pf, c->pidfd >= 0 ? 2 : 1, -1) < 0) { if (errno == EINTR) continue; break; }
        if (pf[1].revents) {
            int st = 0;
            waitpid(c->pid, &st, 0);
            c->pid = 0;
            fprintf(stderr, "ui: the client ended before it connected (%s %d)\n",
                    WIFEXITED(st) ? "exit" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
            c->gone = 1;
            return c;
        }
        if (pf[0].revents & POLLIN) { c->fd = accept(c->lfd, NULL, NULL); break; }
    }
    if (c->fd < 0) { ui_client_stop(c); return NULL; }
    int one = 1;
    setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    char hello[320], want[320];
    snprintf(want, sizeof want, "hello %s", expect);
    if (read_line(c, hello, sizeof hello) < 0 || strcmp(hello, want) != 0) {
        if (strcmp(present, expect) == 0) fprintf(stderr, "ui: the client said \"%s\", not its token\n", hello);
        /* Refused: the connection goes, and with it the client. */
        close(c->fd); c->fd = -1;
        struct pollfd p1 = { c->pidfd, POLLIN, 0 };
        if (c->pidfd >= 0) while (poll(&p1, 1, -1) < 0 && errno == EINTR) {}
        c->gone = 1;
        client_reap(c);
        return c;
    }
    char cmd[64];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"clock\",\"set_ms\":%llu}", (unsigned long long)wall_ms);
    if (ui_cmd(c, cmd, NULL, 0) != 0 || ui_settings_apply(c, s ? s : &UI_DEFAULT) != 0) { ui_client_stop(c); return NULL; }
    return c;
}

ui_client *ui_client_start(const ui_settings *s, uint64_t wall_ms) {
    char tok[40];
    snprintf(tok, sizeof tok, "t%08x%08x", (unsigned)getpid(), (unsigned)random());
    ui_client *c = ui_client_start_tokens(s, wall_ms, tok, tok);
    if (c && c->gone) { ui_client_stop(c); return NULL; }
    return c;
}

int ui_client_gone(ui_client *c) { return c && c->gone; }

void ui_client_stop(ui_client *c) {
    if (!c) return;
    if (c->fd >= 0) { send_all(c->fd, "{\"cmd\":\"quit\"}\n", 15); close(c->fd); }
    if (c->pidfd >= 0) {
        struct pollfd p1 = { c->pidfd, POLLIN, 0 };
        while (poll(&p1, 1, -1) < 0 && errno == EINTR) {}
        close(c->pidfd);
    }
    client_reap(c);
    if (c->lfd >= 0) close(c->lfd);
    free(c);
}

int ui_cmd(ui_client *c, const char *json, char *reply, size_t cap) {
    if (!c || c->fd < 0) return -1;
    const char *tr = getenv("OC_UI_TRACE");
    int trace = tr && !strcmp(tr, "1");
    if (trace) fprintf(stderr, "ui> %s\n", json);
    if (send_all(c->fd, json, strlen(json)) != 0 || send_all(c->fd, "\n", 1) != 0) return -1;
    char line[4096];
    if (read_line(c, line, sizeof line) < 0) return -1;
    if (trace) fprintf(stderr, "ui< %s\n", line);
    if (reply && cap) snprintf(reply, cap, "%s", line);
    const char *f = strstr(line, "\"frame\":");
    if (f) c->frame = strtoull(f + 8, NULL, 10);
    if (!strstr(line, "\"ok\":true")) { fprintf(stderr, "ui: %s -> %s\n", json, line); return -1; }
    return 0;
}

uint64_t ui_frame(const ui_client *c) { return c ? c->frame : 0; }

int ui_settings_apply(ui_client *c, const ui_settings *s) {
    char cmd[320];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"settings\",\"dpi\":%d,\"w\":%d,\"h\":%d,\"tz\":\"%s\",\"theme\":\"%s\",\"textsize\":%d,\"motion\":\"%s\",\"density\":%d}",
             s->dpi, s->w, s->h, s->tz, s->theme, s->textsize, s->motion ? s->motion : "on", s->density);
    return ui_cmd(c, cmd, NULL, 0);
}

int ui_signin(ui_client *c, ui_daemon *d, const char *user, const char *pass) {
    char tok[OC_TEST_TOKEN_CAP], ver[OC_TEST_VERIFIER_CAP], why[256];
    if (oc_test_signin(ui_host(), d->port, d->fp, user, pass, tok, sizeof tok, ver, sizeof ver, why, sizeof why) != 0) {
        fprintf(stderr, "ui: signin %s: %s\n", user, why);
        return -1;
    }
    char fp[2 * OC_TLS_FINGERPRINT_LEN + 1];
    for (int i = 0; i < OC_TLS_FINGERPRINT_LEN; i++) snprintf(fp + 2 * i, 3, "%02x", d->fp[i]);
    char *cmd = malloc(sizeof tok + 640);
    if (!cmd) return -1;
    /* The workspace is named for the port alone, so the screen is the same
     * whatever address the run meets on. */
    snprintf(cmd, sizeof tok + 640,
             "{\"cmd\":\"signin\",\"ws\":\"ui:%d\",\"host\":\"%s\",\"port\":%d,\"fp\":\"%s\","
             "\"source\":\"local\",\"verifier\":\"%s\",\"token\":\"%s\"}", d->port, ui_host(), d->port, fp, ver, tok);
    int rc = ui_cmd(c, cmd, NULL, 0);
    free(cmd);
    return rc;
}

int ui_sync(ui_client *c) { return ui_cmd(c, "{\"cmd\":\"sync\"}", NULL, 0); }

int ui_clock_advance(ui_client *c, uint64_t ms) {
    char cmd[64];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"clock\",\"advance_ms\":%llu}", (unsigned long long)ms);
    return ui_cmd(c, cmd, NULL, 0);
}

int ui_click_id(ui_client *c, const char *id) {
    char cmd[160];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"click\",\"id\":\"%s\"}", id);
    return ui_cmd(c, cmd, NULL, 0);
}

int ui_click_xy(ui_client *c, int x, int y) {
    char cmd[96];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"click\",\"x\":%d,\"y\":%d}", x, y);
    return ui_cmd(c, cmd, NULL, 0);
}

int ui_key(ui_client *c, const char *key, int ctrl, int shift) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"key\",\"key\":\"%s\",\"ctrl\":%d,\"shift\":%d}", key, ctrl, shift);
    return ui_cmd(c, cmd, NULL, 0);
}

int ui_text(ui_client *c, const char *s) {
    char cmd[1024], esc[900];
    size_t o = 0;
    for (const char *p = s; *p && o + 3 < sizeof esc; p++) {
        if (*p == '"' || *p == '\\') esc[o++] = '\\';
        esc[o++] = *p;
    }
    esc[o] = '\0';
    snprintf(cmd, sizeof cmd, "{\"cmd\":\"text\",\"s\":\"%s\"}", esc);
    return ui_cmd(c, cmd, NULL, 0);
}

char *ui_tree(ui_client *c) {
    if (!c || c->fd < 0 || send_all(c->fd, "{\"cmd\":\"tree\"}\n", 15) != 0) return NULL;
    size_t cap = 1 << 22;
    char *line = malloc(cap);
    if (!line) return NULL;
    /* A tree line can be long: read it whole. */
    size_t n = 0;
    for (;;) {
        char *nl = memchr(c->rbuf, '\n', c->rlen);
        size_t take = nl ? (size_t)(nl - c->rbuf) : c->rlen;
        if (n + take >= cap) { free(line); return NULL; }
        memcpy(line + n, c->rbuf, take); n += take;
        size_t drop = nl ? take + 1 : take;
        memmove(c->rbuf, c->rbuf + drop, c->rlen - drop);
        c->rlen -= drop;
        if (nl) break;
        ssize_t r = recv(c->fd, c->rbuf, sizeof c->rbuf, 0);
        if (r <= 0) { free(line); return NULL; }
        c->rlen = (size_t)r;
    }
    line[n] = '\0';
    /* The frame number is the run's, not the screen's: left out. */
    char *f = strstr(line, "\"frame\":");
    if (f) {
        char *e = f + 8;
        while (*e >= '0' && *e <= '9') e++;
        if (*e == ',') e++;
        memmove(f, e, strlen(e) + 1);
    }
    return line;
}

int ui_tree_has(ui_client *c, const char *id, char *name, size_t cap) {
    char *t = ui_tree(c);
    if (!t) return 0;
    char key[160];
    snprintf(key, sizeof key, "\"id\":\"%s\",\"name\":\"", id);
    char *p = strstr(t, key);
    if (p && name && cap) {
        p += strlen(key);
        size_t k = 0;
        while (p[k] && p[k] != '"' && k + 1 < cap) { name[k] = p[k]; k++; }
        name[k] = '\0';
    }
    free(t);
    return p != NULL;
}

uint8_t *ui_shot(ui_client *c, int *w, int *h) {
    if (!c || c->fd < 0 || send_all(c->fd, "{\"cmd\":\"shot\"}\n", 15) != 0) return NULL;
    char line[256];
    if (read_line(c, line, sizeof line) < 0) return NULL;
    const char *pw = strstr(line, "\"w\":"), *ph = strstr(line, "\"h\":"), *pb = strstr(line, "\"bytes\":");
    if (!pw || !ph || !pb) return NULL;
    *w = atoi(pw + 4); *h = atoi(ph + 4);
    size_t n = strtoull(pb + 8, NULL, 10);
    if (!n || n != (size_t)*w * (size_t)*h * 4u) return NULL;
    uint8_t *px = malloc(n);
    if (!px || read_bytes(c, px, n) != 0) { free(px); return NULL; }
    return px;
}

/* ---- references ------------------------------------------------------------- */

static void ref_path(char *out, size_t cap, const char *root, const char *test, const char *step,
                     const ui_settings *s, const char *ext) {
    int moved = s->motion && strcmp(s->motion, "on") != 0;
    snprintf(out, cap, "%s/%s/%s@%ddpi-%dx%d-%s-%s-t%d%s%s%s.%s", root, test, step, s->dpi, s->w, s->h,
             s->theme, s->tz, s->textsize, moved ? "-motion-" : "", moved ? s->motion : "",
             s->density ? "" : "-compact", ext);
}

static void mkdirs(const char *path) {
    char p[600];
    snprintf(p, sizeof p, "%s", path);
    for (char *q = p + 1; *q; q++)
        if (*q == '/') { *q = '\0'; mkdir(p, 0755); *q = '/'; }
}

static int write_file(const char *path, const void *b, size_t n) {
    mkdirs(path);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(b, 1, n, f);
    return fclose(f) == 0 && w == n ? 0 : -1;
}

static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)sz + 1);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[sz] = '\0'; *n = (size_t)sz; }
    return b;
}

static int png_write(const char *path, const uint8_t *px, int w, int h) {
    spng_ctx *ctx = spng_ctx_new(SPNG_CTX_ENCODER);
    struct spng_ihdr ihdr = { 0 };
    ihdr.width = (uint32_t)w; ihdr.height = (uint32_t)h; ihdr.bit_depth = 8;
    ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
    size_t len = 0; int err = 0;
    uint8_t *out = NULL;
    if (ctx && spng_set_option(ctx, SPNG_ENCODE_TO_BUFFER, 1) == 0 && spng_set_ihdr(ctx, &ihdr) == 0 &&
        spng_encode_image(ctx, px, (size_t)w * (size_t)h * 4u, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE) == 0)
        out = spng_get_png_buffer(ctx, &len, &err);
    spng_ctx_free(ctx);
    int rc = (out && !err) ? write_file(path, out, len) : -1;
    free(out);
    return rc;
}

static uint8_t *png_read(const char *path, int *w, int *h) {
    size_t n = 0;
    char *buf = read_file(path, &n);
    if (!buf) return NULL;
    spng_ctx *ctx = spng_ctx_new(0);
    struct spng_ihdr ihdr;
    uint8_t *px = NULL;
    size_t size = 0;
    if (ctx && !spng_set_png_buffer(ctx, buf, n) && !spng_get_ihdr(ctx, &ihdr) &&
        !spng_decoded_image_size(ctx, SPNG_FMT_RGBA8, &size) && (px = malloc(size)) != NULL &&
        spng_decode_image(ctx, px, size, SPNG_FMT_RGBA8, 0) == 0) {
        *w = (int)ihdr.width; *h = (int)ihdr.height;
    } else { free(px); px = NULL; }
    spng_ctx_free(ctx);
    free(buf);
    return px;
}

int ui_check(ui_client *c, const char *test, const char *step, const ui_settings *s) {
    const char *up = getenv("OC_UI_UPDATE");
    int update = up && !strcmp(up, "1");
    char *tree = ui_tree(c);
    int w = 0, h = 0;
    uint8_t *px = ui_shot(c, &w, &h);
    if (!tree || !px) { free(tree); free(px); fprintf(stderr, "ui: %s/%s: no tree or frame\n", test, step); return -1; }
    char rt[600], rp[600], ot[600], op[600];
    ref_path(rt, sizeof rt, "tests/ui/ref", test, step, s, "tree.json");
    ref_path(rp, sizeof rp, "tests/ui/ref", test, step, s, "png");
    ref_path(ot, sizeof ot, "build/ui-out", test, step, s, "tree.json");
    ref_path(op, sizeof op, "build/ui-out", test, step, s, "png");
    int rc = 0;
    if (update) {
        rc = write_file(rt, tree, strlen(tree)) || png_write(rp, px, w, h) ? -1 : 0;
    } else {
        size_t n = 0;
        char *want = read_file(rt, &n);
        int rw = 0, rh = 0;
        uint8_t *rpx = png_read(rp, &rw, &rh);
        if (!want || !rpx) { fprintf(stderr, "ui: %s/%s: no reference %s\n", test, step, !want ? rt : rp); rc = -1; }
        else {
            if (strcmp(want, tree) != 0) { fprintf(stderr, "ui: %s/%s: the tree differs from %s\n", test, step, rt); rc = -1; }
            if (rw != w || rh != h || memcmp(rpx, px, (size_t)w * (size_t)h * 4u) != 0) {
                fprintf(stderr, "ui: %s/%s: the frame differs from %s\n", test, step, rp); rc = -1;
            }
        }
        if (rc) { write_file(ot, tree, strlen(tree)); png_write(op, px, w, h); }
        free(want); free(rpx);
    }
    free(tree); free(px);
    return rc;
}
