/*
 * OpenChime web client — the core in a browser (ARCH-74, CLIENT.md §4), step
 * one: the app-core compiled to WebAssembly, connected to the daemon through
 * its WebSocket transport, signed in, with the model ticking on the page's
 * animation frames. No drawing yet: what it shows, it prints -- the page reads
 * the console, and so does the test (scripts/webtest.sh).
 *
 * The network thread is a pthread (a Web Worker); its socket is Emscripten's,
 * which the page has pointed at the daemon's /ws (index.html). The protocol
 * goes in the clear inside the WebSocket; the page's HTTPS is the wire's TLS
 * (net.c, under __EMSCRIPTEN__).
 */
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client.h"
#include "secret.h"

oc_secret *oc_secret_open_mem(void);   /* secret_mem.c */

static oc_client *g_client;
static oc_secret *g_secret;
static int        g_phase = -1;      /* 0 connecting, 1 connected, 2 authed */
static size_t     g_channels;
static char       g_status[160];

static void tick(void) {
    if (!g_client) return;
    oc_client_tick(g_client);
    const oc_model *m = oc_client_model(g_client);
    int phase = m->authed ? 2 : m->connected ? 1 : 0;
    if (phase != g_phase) {
        g_phase = phase;
        printf("oc-web: %s\n", phase == 2 ? "signed in" : phase == 1 ? "connected" : "connecting");
    }
    if (m->status[0] && strcmp(m->status, g_status) != 0) {
        snprintf(g_status, sizeof g_status, "%s", m->status);
        printf("oc-web: status: %s\n", g_status);
    }
    if (phase == 2 && m->n_channels != g_channels) {
        g_channels = m->n_channels;
        printf("oc-web: %zu channels\n", g_channels);
        for (size_t i = 0; i < m->n_channels && i < 8; i++)
            printf("oc-web:   #%s\n", m->channels[i].name ? m->channels[i].name : "?");
    }
}

/* Start the core against host:port with "user:password". The page calls this
 * once the runtime is up, after pointing Module.websocket.url at the daemon. */
EMSCRIPTEN_KEEPALIVE
int oc_web_start(const char *host, int port, const char *cred) {
    if (g_client) return 0;
    g_secret = oc_secret_open_mem();
    g_client = oc_client_start_named(host, host, port, cred, NULL, g_secret);
    if (!g_client) { printf("oc-web: could not start the core\n"); return -1; }
    printf("oc-web: core started for %s:%d\n", host, port);
    return 0;
}

EMSCRIPTEN_KEEPALIVE
void oc_web_stop(void) {
    if (!g_client) return;
    oc_client_stop(g_client);
    g_client = NULL;
    oc_secret_free(g_secret);
    g_secret = NULL;
    printf("oc-web: stopped\n");
}

/* The sign-in state, for the page and the test: 0 connecting, 1 connected, 2 signed in. */
EMSCRIPTEN_KEEPALIVE
int oc_web_phase(void) { return g_phase; }

EMSCRIPTEN_KEEPALIVE
int oc_web_channels(void) { return (int)g_channels; }

int main(void) {
    emscripten_set_main_loop(tick, 30, 0);
    return 0;
}
