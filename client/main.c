/*
 * OpenChime client — entry point (ARCH-62). One AppCtx + one app_frame(), a
 * native loop driver (openblocks shape), and a spawned network thread. Phase 1:
 * connect to a daemon, complete handshake + stub-auth, show/send messages.
 *
 * Usage: openchime-client [host] [port]
 *   env: OPENCHIME_HOST, OPENCHIME_PORT (default 8443), OPENCHIME_TOKEN.
 */

#include "app.h"
#include "event.h"
#include "gfx.h"
#include "net.h"
#include "queue.h"
#include "ui.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    oc_app   app;
    oc_net  *net;
    oc_queue to_net;    /* UI -> net commands */
    oc_queue from_net;  /* net -> UI events */
} AppCtx;

static void app_frame(AppCtx *ctx) {
    /* Drain network events at the top of the frame (openblocks "events first"). */
    oc_app_drain(&ctx->app);

    int cp;
    while ((cp = gfx_char_pressed()) != 0) oc_app_input_char(&ctx->app, cp);
    if (gfx_backspace_pressed()) oc_app_backspace(&ctx->app);
    if (gfx_enter_pressed())     oc_app_submit(&ctx->app);
    float wheel = gfx_mouse_wheel();
    if (wheel != 0.0f) oc_app_scroll(&ctx->app, wheel * 20.0f);

    gfx_begin_frame();
    oc_ui_draw(&ctx->app);
    gfx_end_frame();
}

int main(int argc, char **argv) {
    const char *host = getenv("OPENCHIME_HOST");
    if (!host) host = "127.0.0.1";
    const char *port_s = getenv("OPENCHIME_PORT");
    int port = port_s ? atoi(port_s) : 8443;
    /* Local credentials as "username:password" until the login UI exists. */
    const char *token = getenv("OPENCHIME_TOKEN");
    if (!token) token = "client:client";
    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    gfx_init(900, 600, "OpenChime");

    AppCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    oc_queue_init(&ctx.to_net);
    oc_queue_init(&ctx.from_net);
    oc_app_init(&ctx.app, &ctx.to_net, &ctx.from_net);
    ctx.net = oc_net_start(host, port, token, &ctx.from_net, &ctx.to_net);

    while (!gfx_should_close()) app_frame(&ctx);

    oc_net_stop(ctx.net);
    /* Drain any late events so their heap bodies are freed. */
    oc_app_drain(&ctx.app);
    oc_app_free(&ctx.app);
    oc_queue_destroy(&ctx.to_net);
    oc_queue_destroy(&ctx.from_net);
    gfx_close();
    return 0;
}
